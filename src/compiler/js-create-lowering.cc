// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/js-create-lowering.h"

#include "src/codegen/code-factory.h"
#include "src/compiler/access-builder.h"
#include "src/compiler/allocation-builder-inl.h"
#include "src/compiler/common-operator.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/js-operator.h"
#include "src/compiler/linkage.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/node.h"
#include "src/compiler/operator-properties.h"
#include "src/compiler/simplified-operator.h"
#include "src/compiler/state-values-utils.h"
#include "src/execution/protectors.h"
#include "src/objects/arguments.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/heap-number.h"
#include "src/objects/js-collection-iterator.h"
#include "src/objects/js-generator.h"
#include "src/objects/js-promise.h"
#include "src/objects/js-regexp-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/template-objects.h"

namespace v8 {
namespace internal {
namespace compiler {

namespace {

// Retrieves the frame state holding actual argument values.
FrameState GetArgumentsFrameState(FrameState frame_state) {
  FrameState outer_state{NodeProperties::GetFrameStateInput(frame_state)};
  return outer_state.frame_state_info().type() ==
                 FrameStateType::kArgumentsAdaptor
             ? outer_state
             : frame_state;
}

// When initializing arrays, we'll unfold the loop if the number of
// elements is known to be of this type.
const int kElementLoopUnrollLimit = 16;

// Limits up to which context allocations are inlined.
const int kFunctionContextAllocationLimit = 16;
const int kBlockContextAllocationLimit = 16;

}  // namespace

Reduction JSCreateLowering::Reduce(Node* node) {
  switch (node->opcode()) {
    case IrOpcode::kJSCreate:
      return ReduceJSCreate(node);
    case IrOpcode::kJSCreateArguments:
      return ReduceJSCreateArguments(node);
    case IrOpcode::kJSCreateArray:
      return ReduceJSCreateArray(node);
    case IrOpcode::kJSCreateArrayIterator:
      return ReduceJSCreateArrayIterator(node);
    case IrOpcode::kJSCreateAsyncFunctionObject:
      return ReduceJSCreateAsyncFunctionObject(node);
    case IrOpcode::kJSCreateBoundFunction:
      return ReduceJSCreateBoundFunction(node);
    case IrOpcode::kJSCreateClosure:
      return ReduceJSCreateClosure(node);
    case IrOpcode::kJSCreateCollectionIterator:
      return ReduceJSCreateCollectionIterator(node);
    case IrOpcode::kJSCreateIterResultObject:
      return ReduceJSCreateIterResultObject(node);
    case IrOpcode::kJSCreateStringIterator:
      return ReduceJSCreateStringIterator(node);
    case IrOpcode::kJSCreateKeyValueArray:
      return ReduceJSCreateKeyValueArray(node);
    case IrOpcode::kJSCreatePromise:
      return ReduceJSCreatePromise(node);
    case IrOpcode::kJSCreateLiteralArray:
    case IrOpcode::kJSCreateLiteralObject:
      return ReduceJSCreateLiteralArrayOrObject(node);
    case IrOpcode::kJSCreateLiteralRegExp:
      return ReduceJSCreateLiteralRegExp(node);
    case IrOpcode::kJSGetTemplateObject:
      return ReduceJSGetTemplateObject(node);
    case IrOpcode::kJSCreateEmptyLiteralArray:
      return ReduceJSCreateEmptyLiteralArray(node);
    case IrOpcode::kJSCreateEmptyLiteralObject:
      return ReduceJSCreateEmptyLiteralObject(node);
    case IrOpcode::kJSCreateFunctionContext:
      return ReduceJSCreateFunctionContext(node);
    case IrOpcode::kJSCreateWithContext:
      return ReduceJSCreateWithContext(node);
    case IrOpcode::kJSCreateCatchContext:
      return ReduceJSCreateCatchContext(node);
    case IrOpcode::kJSCreateBlockContext:
      return ReduceJSCreateBlockContext(node);
    case IrOpcode::kJSCreateGeneratorObject:
      return ReduceJSCreateGeneratorObject(node);
    case IrOpcode::kJSCreateObject:
      return ReduceJSCreateObject(node);
    default:
      break;
  }
  return NoChange();
}

Reduction JSCreateLowering::ReduceJSCreate(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreate, node->opcode());
  Node* const new_target = NodeProperties::GetValueInput(node, 1);
  Node* const effect = NodeProperties::GetEffectInput(node);
  Node* const control = NodeProperties::GetControlInput(node);

  base::Optional<MapRef> initial_map =
      NodeProperties::GetJSCreateMap(broker(), node);
  if (!initial_map.has_value()) return NoChange();

  JSFunctionRef original_constructor =
      HeapObjectMatcher(new_target).Ref(broker()).AsJSFunction();
  SlackTrackingPrediction slack_tracking_prediction =
      dependencies()->DependOnInitialMapInstanceSizePrediction(
          original_constructor);

  // Emit code to allocate the JSObject instance for the
  // {original_constructor}.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(slack_tracking_prediction.instance_size());
  a.Store(AccessBuilder::ForMap(), *initial_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  for (int i = 0; i < slack_tracking_prediction.inobject_property_count();
       ++i) {
    a.Store(AccessBuilder::ForJSObjectInObjectProperty(*initial_map, i),
            jsgraph()->UndefinedConstant());
  }

  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateArguments(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateArguments, node->opcode());
  CreateArgumentsType type = CreateArgumentsTypeOf(node->op());
  FrameState frame_state{NodeProperties::GetFrameStateInput(node)};
  Node* const control = graph()->start();
  FrameStateInfo state_info = frame_state.frame_state_info();
  SharedFunctionInfoRef shared =
      MakeRef(broker(), state_info.shared_info().ToHandleChecked());

  // Use the ArgumentsAccessStub for materializing both mapped and unmapped
  // arguments object, but only for non-inlined (i.e. outermost) frames.
  if (!frame_state.has_outer_frame_state()) {
    switch (type) {
      case CreateArgumentsType::kMappedArguments: {
        // TODO(turbofan): Duplicate parameters are not handled yet.
        if (shared.has_duplicate_parameters()) return NoChange();
        Node* const callee = NodeProperties::GetValueInput(node, 0);
        Node* const context = NodeProperties::GetContextInput(node);
        Node* effect = NodeProperties::GetEffectInput(node);
        Node* const arguments_length =
            graph()->NewNode(simplified()->ArgumentsLength());
        // Allocate the elements backing store.
        bool has_aliased_arguments = false;
        Node* const elements = effect = TryAllocateAliasedArguments(
            effect, control, context, arguments_length, shared,
            &has_aliased_arguments);
        if (elements == nullptr) return NoChange();

        // Load the arguments object map.
        Node* const arguments_map = jsgraph()->Constant(
            has_aliased_arguments
                ? native_context().fast_aliased_arguments_map()
                : native_context().sloppy_arguments_map());
        // Actually allocate and initialize the arguments object.
        AllocationBuilder a(jsgraph(), effect, control);
        STATIC_ASSERT(JSSloppyArgumentsObject::kSize == 5 * kTaggedSize);
        a.Allocate(JSSloppyArgumentsObject::kSize);
        a.Store(AccessBuilder::ForMap(), arguments_map);
        a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
                jsgraph()->EmptyFixedArrayConstant());
        a.Store(AccessBuilder::ForJSObjectElements(), elements);
        a.Store(AccessBuilder::ForArgumentsLength(), arguments_length);
        a.Store(AccessBuilder::ForArgumentsCallee(), callee);
        RelaxControls(node);
        a.FinishAndChange(node);
        return Changed(node);
      }
      case CreateArgumentsType::kUnmappedArguments: {
        Node* effect = NodeProperties::GetEffectInput(node);
        Node* const arguments_length =
            graph()->NewNode(simplified()->ArgumentsLength());
        // Allocate the elements backing store.
        Node* const elements = effect =
            graph()->NewNode(simplified()->NewArgumentsElements(
                                 CreateArgumentsType::kUnmappedArguments,
                                 shared.internal_formal_parameter_count()),
                             arguments_length, effect);
        // Load the arguments object map.
        Node* const arguments_map =
            jsgraph()->Constant(native_context().strict_arguments_map());
        // Actually allocate and initialize the arguments object.
        AllocationBuilder a(jsgraph(), effect, control);
        STATIC_ASSERT(JSStrictArgumentsObject::kSize == 4 * kTaggedSize);
        a.Allocate(JSStrictArgumentsObject::kSize);
        a.Store(AccessBuilder::ForMap(), arguments_map);
        a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
                jsgraph()->EmptyFixedArrayConstant());
        a.Store(AccessBuilder::ForJSObjectElements(), elements);
        a.Store(AccessBuilder::ForArgumentsLength(), arguments_length);
        RelaxControls(node);
        a.FinishAndChange(node);
        return Changed(node);
      }
      case CreateArgumentsType::kRestParameter: {
        Node* effect = NodeProperties::GetEffectInput(node);
        Node* const arguments_length =
            graph()->NewNode(simplified()->ArgumentsLength());
        Node* const rest_length = graph()->NewNode(
            simplified()->RestLength(shared.internal_formal_parameter_count()));
        // Allocate the elements backing store.
        Node* const elements = effect =
            graph()->NewNode(simplified()->NewArgumentsElements(
                                 CreateArgumentsType::kRestParameter,
                                 shared.internal_formal_parameter_count()),
                             arguments_length, effect);
        // Load the JSArray object map.
        Node* const jsarray_map = jsgraph()->Constant(
            native_context().js_array_packed_elements_map());
        // Actually allocate and initialize the jsarray.
        AllocationBuilder a(jsgraph(), effect, control);
        STATIC_ASSERT(JSArray::kHeaderSize == 4 * kTaggedSize);
        a.Allocate(JSArray::kHeaderSize);
        a.Store(AccessBuilder::ForMap(), jsarray_map);
        a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
                jsgraph()->EmptyFixedArrayConstant());
        a.Store(AccessBuilder::ForJSObjectElements(), elements);
        a.Store(AccessBuilder::ForJSArrayLength(PACKED_ELEMENTS), rest_length);
        RelaxControls(node);
        a.FinishAndChange(node);
        return Changed(node);
      }
    }
    UNREACHABLE();
  }
  // Use inline allocation for all mapped arguments objects within inlined
  // (i.e. non-outermost) frames, independent of the object size.
  DCHECK_EQ(frame_state.outer_frame_state()->opcode(), IrOpcode::kFrameState);
  switch (type) {
    case CreateArgumentsType::kMappedArguments: {
      Node* const callee = NodeProperties::GetValueInput(node, 0);
      Node* const context = NodeProperties::GetContextInput(node);
      Node* effect = NodeProperties::GetEffectInput(node);
      // TODO(turbofan): Duplicate parameters are not handled yet.
      if (shared.has_duplicate_parameters()) return NoChange();
      // Choose the correct frame state and frame state info depending on
      // whether there conceptually is an arguments adaptor frame in the call
      // chain.
      FrameState args_state = GetArgumentsFrameState(frame_state);
      if (args_state.parameters()->opcode() == IrOpcode::kDeadValue) {
        // This protects against an incompletely propagated DeadValue node.
        // If the FrameState has a DeadValue input, then this node will be
        // pruned anyway.
        return NoChange();
      }
      FrameStateInfo args_state_info = args_state.frame_state_info();
      int length = args_state_info.parameter_count() - 1;  // Minus receiver.
      // Prepare element backing store to be used by arguments object.
      bool has_aliased_arguments = false;
      Node* const elements = TryAllocateAliasedArguments(
          effect, control, args_state, context, shared, &has_aliased_arguments);
      if (elements == nullptr) return NoChange();
      effect = elements->op()->EffectOutputCount() > 0 ? elements : effect;
      // Load the arguments object map.
      Node* const arguments_map = jsgraph()->Constant(
          has_aliased_arguments ? native_context().fast_aliased_arguments_map()
                                : native_context().sloppy_arguments_map());
      // Actually allocate and initialize the arguments object.
      AllocationBuilder a(jsgraph(), effect, control);
      STATIC_ASSERT(JSSloppyArgumentsObject::kSize == 5 * kTaggedSize);
      a.Allocate(JSSloppyArgumentsObject::kSize);
      a.Store(AccessBuilder::ForMap(), arguments_map);
      a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
              jsgraph()->EmptyFixedArrayConstant());
      a.Store(AccessBuilder::ForJSObjectElements(), elements);
      a.Store(AccessBuilder::ForArgumentsLength(), jsgraph()->Constant(length));
      a.Store(AccessBuilder::ForArgumentsCallee(), callee);
      RelaxControls(node);
      a.FinishAndChange(node);
      return Changed(node);
    }
    case CreateArgumentsType::kUnmappedArguments: {
      // Use inline allocation for all unmapped arguments objects within inlined
      // (i.e. non-outermost) frames, independent of the object size.
      Node* effect = NodeProperties::GetEffectInput(node);
      // Choose the correct frame state and frame state info depending on
      // whether there conceptually is an arguments adaptor frame in the call
      // chain.
      FrameState args_state = GetArgumentsFrameState(frame_state);
      if (args_state.parameters()->opcode() == IrOpcode::kDeadValue) {
        // This protects against an incompletely propagated DeadValue node.
        // If the FrameState has a DeadValue input, then this node will be
        // pruned anyway.
        return NoChange();
      }
      FrameStateInfo args_state_info = args_state.frame_state_info();
      int length = args_state_info.parameter_count() - 1;  // Minus receiver.
      // Prepare element backing store to be used by arguments object.
      Node* const elements = TryAllocateArguments(effect, control, args_state);
      if (elements == nullptr) return NoChange();
      effect = elements->op()->EffectOutputCount() > 0 ? elements : effect;
      // Load the arguments object map.
      Node* const arguments_map =
          jsgraph()->Constant(native_context().strict_arguments_map());
      // Actually allocate and initialize the arguments object.
      AllocationBuilder a(jsgraph(), effect, control);
      STATIC_ASSERT(JSStrictArgumentsObject::kSize == 4 * kTaggedSize);
      a.Allocate(JSStrictArgumentsObject::kSize);
      a.Store(AccessBuilder::ForMap(), arguments_map);
      a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
              jsgraph()->EmptyFixedArrayConstant());
      a.Store(AccessBuilder::ForJSObjectElements(), elements);
      a.Store(AccessBuilder::ForArgumentsLength(), jsgraph()->Constant(length));
      RelaxControls(node);
      a.FinishAndChange(node);
      return Changed(node);
    }
    case CreateArgumentsType::kRestParameter: {
      int start_index = shared.internal_formal_parameter_count();
      // Use inline allocation for all unmapped arguments objects within inlined
      // (i.e. non-outermost) frames, independent of the object size.
      Node* effect = NodeProperties::GetEffectInput(node);
      // Choose the correct frame state and frame state info depending on
      // whether there conceptually is an arguments adaptor frame in the call
      // chain.
      FrameState args_state = GetArgumentsFrameState(frame_state);
      if (args_state.parameters()->opcode() == IrOpcode::kDeadValue) {
        // This protects against an incompletely propagated DeadValue node.
        // If the FrameState has a DeadValue input, then this node will be
        // pruned anyway.
        return NoChange();
      }
      FrameStateInfo args_state_info = args_state.frame_state_info();
      // Prepare element backing store to be used by the rest array.
      Node* const elements =
          TryAllocateRestArguments(effect, control, args_state, start_index);
      if (elements == nullptr) return NoChange();
      effect = elements->op()->EffectOutputCount() > 0 ? elements : effect;
      // Load the JSArray object map.
      Node* const jsarray_map =
          jsgraph()->Constant(native_context().js_array_packed_elements_map());
      // Actually allocate and initialize the jsarray.
      AllocationBuilder a(jsgraph(), effect, control);

      // -1 to minus receiver
      int argument_count = args_state_info.parameter_count() - 1;
      int length = std::max(0, argument_count - start_index);
      STATIC_ASSERT(JSArray::kHeaderSize == 4 * kTaggedSize);
      a.Allocate(JSArray::kHeaderSize);
      a.Store(AccessBuilder::ForMap(), jsarray_map);
      a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
              jsgraph()->EmptyFixedArrayConstant());
      a.Store(AccessBuilder::ForJSObjectElements(), elements);
      a.Store(AccessBuilder::ForJSArrayLength(PACKED_ELEMENTS),
              jsgraph()->Constant(length));
      RelaxControls(node);
      a.FinishAndChange(node);
      return Changed(node);
    }
  }
  UNREACHABLE();
}

Reduction JSCreateLowering::ReduceJSCreateGeneratorObject(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateGeneratorObject, node->opcode());
  Node* const closure = NodeProperties::GetValueInput(node, 0);
  Node* const receiver = NodeProperties::GetValueInput(node, 1);
  Node* const context = NodeProperties::GetContextInput(node);
  Type const closure_type = NodeProperties::GetType(closure);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* const control = NodeProperties::GetControlInput(node);
  if (closure_type.IsHeapConstant()) {
    DCHECK(closure_type.AsHeapConstant()->Ref().IsJSFunction());
    JSFunctionRef js_function =
        closure_type.AsHeapConstant()->Ref().AsJSFunction();
    if (!js_function.has_initial_map()) return NoChange();

    SlackTrackingPrediction slack_tracking_prediction =
        dependencies()->DependOnInitialMapInstanceSizePrediction(js_function);

    MapRef initial_map = js_function.initial_map();
    DCHECK(initial_map.instance_type() == JS_GENERATOR_OBJECT_TYPE ||
           initial_map.instance_type() == JS_ASYNC_GENERATOR_OBJECT_TYPE);

    // Allocate a register file.
    SharedFunctionInfoRef shared = js_function.shared();
    DCHECK(shared.HasBytecodeArray());
    int parameter_count_no_receiver = shared.internal_formal_parameter_count();
    int length = parameter_count_no_receiver +
                 shared.GetBytecodeArray().register_count();
    MapRef fixed_array_map = MakeRef(broker(), factory()->fixed_array_map());
    AllocationBuilder ab(jsgraph(), effect, control);
    if (!ab.CanAllocateArray(length, fixed_array_map)) {
      return NoChange();
    }
    ab.AllocateArray(length, fixed_array_map);
    for (int i = 0; i < length; ++i) {
      ab.Store(AccessBuilder::ForFixedArraySlot(i),
               jsgraph()->UndefinedConstant());
    }
    Node* parameters_and_registers = effect = ab.Finish();

    // Emit code to allocate the JS[Async]GeneratorObject instance.
    AllocationBuilder a(jsgraph(), effect, control);
    a.Allocate(slack_tracking_prediction.instance_size());
    Node* undefined = jsgraph()->UndefinedConstant();
    a.Store(AccessBuilder::ForMap(), initial_map);
    a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
            jsgraph()->EmptyFixedArrayConstant());
    a.Store(AccessBuilder::ForJSObjectElements(),
            jsgraph()->EmptyFixedArrayConstant());
    a.Store(AccessBuilder::ForJSGeneratorObjectContext(), context);
    a.Store(AccessBuilder::ForJSGeneratorObjectFunction(), closure);
    a.Store(AccessBuilder::ForJSGeneratorObjectReceiver(), receiver);
    a.Store(AccessBuilder::ForJSGeneratorObjectInputOrDebugPos(), undefined);
    a.Store(AccessBuilder::ForJSGeneratorObjectResumeMode(),
            jsgraph()->Constant(JSGeneratorObject::kNext));
    a.Store(AccessBuilder::ForJSGeneratorObjectContinuation(),
            jsgraph()->Constant(JSGeneratorObject::kGeneratorExecuting));
    a.Store(AccessBuilder::ForJSGeneratorObjectParametersAndRegisters(),
            parameters_and_registers);

    if (initial_map.instance_type() == JS_ASYNC_GENERATOR_OBJECT_TYPE) {
      a.Store(AccessBuilder::ForJSAsyncGeneratorObjectQueue(), undefined);
      a.Store(AccessBuilder::ForJSAsyncGeneratorObjectIsAwaiting(),
              jsgraph()->ZeroConstant());
    }

    // Handle in-object properties, too.
    for (int i = 0; i < slack_tracking_prediction.inobject_property_count();
         ++i) {
      a.Store(AccessBuilder::ForJSObjectInObjectProperty(initial_map, i),
              undefined);
    }
    a.FinishAndChange(node);
    return Changed(node);
  }
  return NoChange();
}

// Constructs an array with a variable {length} when no upper bound
// is known for the capacity.
Reduction JSCreateLowering::ReduceNewArray(
    Node* node, Node* length, MapRef initial_map, ElementsKind elements_kind,
    AllocationType allocation,
    const SlackTrackingPrediction& slack_tracking_prediction) {
  DCHECK_EQ(IrOpcode::kJSCreateArray, node->opcode());
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Constructing an Array via new Array(N) where N is an unsigned
  // integer, always creates a holey backing store.
  ASSIGN_RETURN_NO_CHANGE_IF_DATA_MISSING(
      initial_map,
      initial_map.AsElementsKind(GetHoleyElementsKind(elements_kind)));

  // Because CheckBounds performs implicit conversion from string to number, an
  // additional CheckNumber is required to behave correctly for calls with a
  // single string argument.
  length = effect = graph()->NewNode(
      simplified()->CheckNumber(FeedbackSource{}), length, effect, control);

  // Check that the {limit} is an unsigned integer in the valid range.
  // This has to be kept in sync with src/runtime/runtime-array.cc,
  // where this limit is protected.
  length = effect = graph()->NewNode(
      simplified()->CheckBounds(FeedbackSource()), length,
      jsgraph()->Constant(JSArray::kInitialMaxFastElementArray), effect,
      control);

  // Construct elements and properties for the resulting JSArray.
  Node* elements = effect =
      graph()->NewNode(IsDoubleElementsKind(initial_map.elements_kind())
                           ? simplified()->NewDoubleElements(allocation)
                           : simplified()->NewSmiOrObjectElements(allocation),
                       length, effect, control);

  // Perform the allocation of the actual JSArray object.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(slack_tracking_prediction.instance_size(), allocation);
  a.Store(AccessBuilder::ForMap(), initial_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(), elements);
  a.Store(AccessBuilder::ForJSArrayLength(initial_map.elements_kind()), length);
  for (int i = 0; i < slack_tracking_prediction.inobject_property_count();
       ++i) {
    a.Store(AccessBuilder::ForJSObjectInObjectProperty(initial_map, i),
            jsgraph()->UndefinedConstant());
  }
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

// Constructs an array with a variable {length} when an actual
// upper bound is known for the {capacity}.
Reduction JSCreateLowering::ReduceNewArray(
    Node* node, Node* length, int capacity, MapRef initial_map,
    ElementsKind elements_kind, AllocationType allocation,
    const SlackTrackingPrediction& slack_tracking_prediction) {
  DCHECK(node->opcode() == IrOpcode::kJSCreateArray ||
         node->opcode() == IrOpcode::kJSCreateEmptyLiteralArray);
  DCHECK(NodeProperties::GetType(length).Is(Type::Number()));
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Determine the appropriate elements kind.
  if (NodeProperties::GetType(length).Max() > 0.0) {
    elements_kind = GetHoleyElementsKind(elements_kind);
  }
  ASSIGN_RETURN_NO_CHANGE_IF_DATA_MISSING(
      initial_map, initial_map.AsElementsKind(elements_kind));
  DCHECK(IsFastElementsKind(elements_kind));

  // Setup elements and properties.
  Node* elements;
  if (capacity == 0) {
    elements = jsgraph()->EmptyFixedArrayConstant();
  } else {
    elements = effect =
        AllocateElements(effect, control, elements_kind, capacity, allocation);
  }

  // Perform the allocation of the actual JSArray object.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(slack_tracking_prediction.instance_size(), allocation);
  a.Store(AccessBuilder::ForMap(), initial_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(), elements);
  a.Store(AccessBuilder::ForJSArrayLength(elements_kind), length);
  for (int i = 0; i < slack_tracking_prediction.inobject_property_count();
       ++i) {
    a.Store(AccessBuilder::ForJSObjectInObjectProperty(initial_map, i),
            jsgraph()->UndefinedConstant());
  }
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceNewArray(
    Node* node, std::vector<Node*> values, MapRef initial_map,
    ElementsKind elements_kind, AllocationType allocation,
    const SlackTrackingPrediction& slack_tracking_prediction) {
  DCHECK_EQ(IrOpcode::kJSCreateArray, node->opcode());
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Determine the appropriate elements kind.
  DCHECK(IsFastElementsKind(elements_kind));
  ASSIGN_RETURN_NO_CHANGE_IF_DATA_MISSING(
      initial_map, initial_map.AsElementsKind(elements_kind));

  // Check {values} based on the {elements_kind}. These checks are guarded
  // by the {elements_kind} feedback on the {site}, so it's safe to just
  // deoptimize in this case.
  if (IsSmiElementsKind(elements_kind)) {
    for (auto& value : values) {
      if (!NodeProperties::GetType(value).Is(Type::SignedSmall())) {
        value = effect = graph()->NewNode(
            simplified()->CheckSmi(FeedbackSource()), value, effect, control);
      }
    }
  } else if (IsDoubleElementsKind(elements_kind)) {
    for (auto& value : values) {
      if (!NodeProperties::GetType(value).Is(Type::Number())) {
        value = effect =
            graph()->NewNode(simplified()->CheckNumber(FeedbackSource()), value,
                             effect, control);
      }
      // Make sure we do not store signaling NaNs into double arrays.
      value = graph()->NewNode(simplified()->NumberSilenceNaN(), value);
    }
  }

  // Setup elements, properties and length.
  Node* elements = effect =
      AllocateElements(effect, control, elements_kind, values, allocation);
  Node* length = jsgraph()->Constant(static_cast<int>(values.size()));

  // Perform the allocation of the actual JSArray object.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(slack_tracking_prediction.instance_size(), allocation);
  a.Store(AccessBuilder::ForMap(), initial_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(), elements);
  a.Store(AccessBuilder::ForJSArrayLength(elements_kind), length);
  for (int i = 0; i < slack_tracking_prediction.inobject_property_count();
       ++i) {
    a.Store(AccessBuilder::ForJSObjectInObjectProperty(initial_map, i),
            jsgraph()->UndefinedConstant());
  }
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateArray(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateArray, node->opcode());
  CreateArrayParameters const& p = CreateArrayParametersOf(node->op());
  int const arity = static_cast<int>(p.arity());
  base::Optional<AllocationSiteRef> site_ref;
  {
    Handle<AllocationSite> site;
    if (p.site().ToHandle(&site)) {
      site_ref = MakeRef(broker(), site);
    }
  }
  AllocationType allocation = AllocationType::kYoung;

  base::Optional<MapRef> initial_map =
      NodeProperties::GetJSCreateMap(broker(), node);
  if (!initial_map.has_value()) return NoChange();

  Node* new_target = NodeProperties::GetValueInput(node, 1);
  JSFunctionRef original_constructor =
      HeapObjectMatcher(new_target).Ref(broker()).AsJSFunction();
  SlackTrackingPrediction slack_tracking_prediction =
      dependencies()->DependOnInitialMapInstanceSizePrediction(
          original_constructor);

  // Tells whether we are protected by either the {site} or a
  // protector cell to do certain speculative optimizations.
  bool can_inline_call = false;

  // Check if we have a feedback {site} on the {node}.
  ElementsKind elements_kind = initial_map->elements_kind();
  if (site_ref) {
    elements_kind = site_ref->GetElementsKind();
    can_inline_call = site_ref->CanInlineCall();
    allocation = dependencies()->DependOnPretenureMode(*site_ref);
    dependencies()->DependOnElementsKind(*site_ref);
  } else {
    PropertyCellRef array_constructor_protector =
        MakeRef(broker(), factory()->array_constructor_protector());
    array_constructor_protector.SerializeAsProtector();
    can_inline_call = array_constructor_protector.value().AsSmi() ==
                      Protectors::kProtectorValid;
  }

  if (arity == 0) {
    Node* length = jsgraph()->ZeroConstant();
    int capacity = JSArray::kPreallocatedArrayElements;
    return ReduceNewArray(node, length, capacity, *initial_map, elements_kind,
                          allocation, slack_tracking_prediction);
  } else if (arity == 1) {
    Node* length = NodeProperties::GetValueInput(node, 2);
    Type length_type = NodeProperties::GetType(length);
    if (!length_type.Maybe(Type::Number())) {
      // Handle the single argument case, where we know that the value
      // cannot be a valid Array length.
      elements_kind = GetMoreGeneralElementsKind(
          elements_kind, IsHoleyElementsKind(elements_kind) ? HOLEY_ELEMENTS
                                                            : PACKED_ELEMENTS);
      return ReduceNewArray(node, std::vector<Node*>{length}, *initial_map,
                            elements_kind, allocation,
                            slack_tracking_prediction);
    }
    if (length_type.Is(Type::SignedSmall()) && length_type.Min() >= 0 &&
        length_type.Max() <= kElementLoopUnrollLimit &&
        length_type.Min() == length_type.Max()) {
      int capacity = static_cast<int>(length_type.Max());
      // Replace length with a constant in order to protect against a potential
      // typer bug leading to length > capacity.
      length = jsgraph()->Constant(capacity);
      return ReduceNewArray(node, length, capacity, *initial_map, elements_kind,
                            allocation, slack_tracking_prediction);
    }
    if (length_type.Maybe(Type::UnsignedSmall()) && can_inline_call) {
      return ReduceNewArray(node, length, *initial_map, elements_kind,
                            allocation, slack_tracking_prediction);
    }
  } else if (arity <= JSArray::kInitialMaxFastElementArray) {
    // Gather the values to store into the newly created array.
    bool values_all_smis = true, values_all_numbers = true,
         values_any_nonnumber = false;
    std::vector<Node*> values;
    values.reserve(p.arity());
    for (int i = 0; i < arity; ++i) {
      Node* value = NodeProperties::GetValueInput(node, 2 + i);
      Type value_type = NodeProperties::GetType(value);
      if (!value_type.Is(Type::SignedSmall())) {
        values_all_smis = false;
      }
      if (!value_type.Is(Type::Number())) {
        values_all_numbers = false;
      }
      if (!value_type.Maybe(Type::Number())) {
        values_any_nonnumber = true;
      }
      values.push_back(value);
    }

    // Try to figure out the ideal elements kind statically.
    if (values_all_smis) {
      // Smis can be stored with any elements kind.
    } else if (values_all_numbers) {
      elements_kind = GetMoreGeneralElementsKind(
          elements_kind, IsHoleyElementsKind(elements_kind)
                             ? HOLEY_DOUBLE_ELEMENTS
                             : PACKED_DOUBLE_ELEMENTS);
    } else if (values_any_nonnumber) {
      elements_kind = GetMoreGeneralElementsKind(
          elements_kind, IsHoleyElementsKind(elements_kind) ? HOLEY_ELEMENTS
                                                            : PACKED_ELEMENTS);
    } else if (!can_inline_call) {
      // We have some crazy combination of types for the {values} where
      // there's no clear decision on the elements kind statically. And
      // we don't have a protection against deoptimization loops for the
      // checks that are introduced in the call to ReduceNewArray, so
      // we cannot inline this invocation of the Array constructor here.
      return NoChange();
    }
    return ReduceNewArray(node, values, *initial_map, elements_kind, allocation,
                          slack_tracking_prediction);
  }
  return NoChange();
}

Reduction JSCreateLowering::ReduceJSCreateArrayIterator(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateArrayIterator, node->opcode());
  CreateArrayIteratorParameters const& p =
      CreateArrayIteratorParametersOf(node->op());
  Node* iterated_object = NodeProperties::GetValueInput(node, 0);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Create the JSArrayIterator result.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(JSArrayIterator::kHeaderSize, AllocationType::kYoung,
             Type::OtherObject());
  a.Store(AccessBuilder::ForMap(),
          native_context().initial_array_iterator_map());
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSArrayIteratorIteratedObject(), iterated_object);
  a.Store(AccessBuilder::ForJSArrayIteratorNextIndex(),
          jsgraph()->ZeroConstant());
  a.Store(AccessBuilder::ForJSArrayIteratorKind(),
          jsgraph()->Constant(static_cast<int>(p.kind())));
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateAsyncFunctionObject(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateAsyncFunctionObject, node->opcode());
  int const register_count = RegisterCountOf(node->op());
  Node* closure = NodeProperties::GetValueInput(node, 0);
  Node* receiver = NodeProperties::GetValueInput(node, 1);
  Node* promise = NodeProperties::GetValueInput(node, 2);
  Node* context = NodeProperties::GetContextInput(node);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Create the register file.
  MapRef fixed_array_map = MakeRef(broker(), factory()->fixed_array_map());
  AllocationBuilder ab(jsgraph(), effect, control);
  CHECK(ab.CanAllocateArray(register_count, fixed_array_map));
  ab.AllocateArray(register_count, fixed_array_map);
  for (int i = 0; i < register_count; ++i) {
    ab.Store(AccessBuilder::ForFixedArraySlot(i),
             jsgraph()->UndefinedConstant());
  }
  Node* parameters_and_registers = effect = ab.Finish();

  // Create the JSAsyncFunctionObject result.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(JSAsyncFunctionObject::kHeaderSize);
  a.Store(AccessBuilder::ForMap(),
          native_context().async_function_object_map());
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSGeneratorObjectContext(), context);
  a.Store(AccessBuilder::ForJSGeneratorObjectFunction(), closure);
  a.Store(AccessBuilder::ForJSGeneratorObjectReceiver(), receiver);
  a.Store(AccessBuilder::ForJSGeneratorObjectInputOrDebugPos(),
          jsgraph()->UndefinedConstant());
  a.Store(AccessBuilder::ForJSGeneratorObjectResumeMode(),
          jsgraph()->Constant(JSGeneratorObject::kNext));
  a.Store(AccessBuilder::ForJSGeneratorObjectContinuation(),
          jsgraph()->Constant(JSGeneratorObject::kGeneratorExecuting));
  a.Store(AccessBuilder::ForJSGeneratorObjectParametersAndRegisters(),
          parameters_and_registers);
  a.Store(AccessBuilder::ForJSAsyncFunctionObjectPromise(), promise);
  a.FinishAndChange(node);
  return Changed(node);
}

namespace {

MapRef MapForCollectionIterationKind(const NativeContextRef& native_context,
                                     CollectionKind collection_kind,
                                     IterationKind iteration_kind) {
  switch (collection_kind) {
    case CollectionKind::kSet:
      switch (iteration_kind) {
        case IterationKind::kKeys:
          UNREACHABLE();
        case IterationKind::kValues:
          return native_context.set_value_iterator_map();
        case IterationKind::kEntries:
          return native_context.set_key_value_iterator_map();
      }
      break;
    case CollectionKind::kMap:
      switch (iteration_kind) {
        case IterationKind::kKeys:
          return native_context.map_key_iterator_map();
        case IterationKind::kValues:
          return native_context.map_value_iterator_map();
        case IterationKind::kEntries:
          return native_context.map_key_value_iterator_map();
      }
      break;
  }
  UNREACHABLE();
}

}  // namespace

Reduction JSCreateLowering::ReduceJSCreateCollectionIterator(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateCollectionIterator, node->opcode());
  CreateCollectionIteratorParameters const& p =
      CreateCollectionIteratorParametersOf(node->op());
  Node* iterated_object = NodeProperties::GetValueInput(node, 0);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Load the OrderedHashTable from the {receiver}.
  Node* table = effect = graph()->NewNode(
      simplified()->LoadField(AccessBuilder::ForJSCollectionTable()),
      iterated_object, effect, control);

  // Create the JSCollectionIterator result.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(JSCollectionIterator::kHeaderSize, AllocationType::kYoung,
             Type::OtherObject());
  a.Store(AccessBuilder::ForMap(),
          MapForCollectionIterationKind(native_context(), p.collection_kind(),
                                        p.iteration_kind()));
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSCollectionIteratorTable(), table);
  a.Store(AccessBuilder::ForJSCollectionIteratorIndex(),
          jsgraph()->ZeroConstant());
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateBoundFunction(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateBoundFunction, node->opcode());
  CreateBoundFunctionParameters const& p =
      CreateBoundFunctionParametersOf(node->op());
  int const arity = static_cast<int>(p.arity());
  MapRef const map = MakeRef(broker(), p.map());
  Node* bound_target_function = NodeProperties::GetValueInput(node, 0);
  Node* bound_this = NodeProperties::GetValueInput(node, 1);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Create the [[BoundArguments]] for the result.
  Node* bound_arguments = jsgraph()->EmptyFixedArrayConstant();
  if (arity > 0) {
    MapRef fixed_array_map = MakeRef(broker(), factory()->fixed_array_map());
    AllocationBuilder ab(jsgraph(), effect, control);
    CHECK(ab.CanAllocateArray(arity, fixed_array_map));
    ab.AllocateArray(arity, fixed_array_map);
    for (int i = 0; i < arity; ++i) {
      ab.Store(AccessBuilder::ForFixedArraySlot(i),
               NodeProperties::GetValueInput(node, 2 + i));
    }
    bound_arguments = effect = ab.Finish();
  }

  // Create the JSBoundFunction result.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(JSBoundFunction::kHeaderSize, AllocationType::kYoung,
             Type::BoundFunction());
  a.Store(AccessBuilder::ForMap(), map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSBoundFunctionBoundTargetFunction(),
          bound_target_function);
  a.Store(AccessBuilder::ForJSBoundFunctionBoundThis(), bound_this);
  a.Store(AccessBuilder::ForJSBoundFunctionBoundArguments(), bound_arguments);
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateClosure(Node* node) {
  JSCreateClosureNode n(node);
  CreateClosureParameters const& p = n.Parameters();
  SharedFunctionInfoRef shared = MakeRef(broker(), p.shared_info());
  FeedbackCellRef feedback_cell = n.GetFeedbackCellRefChecked(broker());
  HeapObjectRef code = MakeRef(broker(), p.code());
  Effect effect = n.effect();
  Control control = n.control();
  Node* context = n.context();

  // Use inline allocation of closures only for instantiation sites that have
  // seen more than one instantiation, this simplifies the generated code and
  // also serves as a heuristic of which allocation sites benefit from it.
  if (!feedback_cell.map().equals(
          MakeRef(broker(), factory()->many_closures_cell_map()))) {
    return NoChange();
  }

  MapRef function_map =
      native_context().GetFunctionMapFromIndex(shared.function_map_index());
  DCHECK(!function_map.IsInobjectSlackTrackingInProgress());
  DCHECK(!function_map.is_dictionary_map());

  // TODO(turbofan): We should use the pretenure flag from {p} here,
  // but currently the heuristic in the parser works against us, as
  // it marks closures like
  //
  //   args[l] = function(...) { ... }
  //
  // for old-space allocation, which doesn't always make sense. For
  // example in case of the bluebird-parallel benchmark, where this
  // is a core part of the *promisify* logic (see crbug.com/810132).
  AllocationType allocation = AllocationType::kYoung;

  // Emit code to allocate the JSFunction instance.
  STATIC_ASSERT(JSFunction::kSizeWithoutPrototype == 7 * kTaggedSize);
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(function_map.instance_size(), allocation, Type::Function());
  a.Store(AccessBuilder::ForMap(), function_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSFunctionSharedFunctionInfo(), shared);
  a.Store(AccessBuilder::ForJSFunctionContext(), context);
  a.Store(AccessBuilder::ForJSFunctionFeedbackCell(), feedback_cell);
  a.Store(AccessBuilder::ForJSFunctionCode(), code);
  STATIC_ASSERT(JSFunction::kSizeWithoutPrototype == 7 * kTaggedSize);
  if (function_map.has_prototype_slot()) {
    a.Store(AccessBuilder::ForJSFunctionPrototypeOrInitialMap(),
            jsgraph()->TheHoleConstant());
    STATIC_ASSERT(JSFunction::kSizeWithPrototype == 8 * kTaggedSize);
  }
  for (int i = 0; i < function_map.GetInObjectProperties(); i++) {
    a.Store(AccessBuilder::ForJSObjectInObjectProperty(function_map, i),
            jsgraph()->UndefinedConstant());
  }
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateIterResultObject(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateIterResultObject, node->opcode());
  Node* value = NodeProperties::GetValueInput(node, 0);
  Node* done = NodeProperties::GetValueInput(node, 1);
  Node* effect = NodeProperties::GetEffectInput(node);

  Node* iterator_result_map =
      jsgraph()->Constant(native_context().iterator_result_map());

  // Emit code to allocate the JSIteratorResult instance.
  AllocationBuilder a(jsgraph(), effect, graph()->start());
  a.Allocate(JSIteratorResult::kSize);
  a.Store(AccessBuilder::ForMap(), iterator_result_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSIteratorResultValue(), value);
  a.Store(AccessBuilder::ForJSIteratorResultDone(), done);
  STATIC_ASSERT(JSIteratorResult::kSize == 5 * kTaggedSize);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateStringIterator(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateStringIterator, node->opcode());
  Node* string = NodeProperties::GetValueInput(node, 0);
  Node* effect = NodeProperties::GetEffectInput(node);

  Node* map =
      jsgraph()->Constant(native_context().initial_string_iterator_map());
  // Allocate new iterator and attach the iterator to this string.
  AllocationBuilder a(jsgraph(), effect, graph()->start());
  a.Allocate(JSStringIterator::kHeaderSize, AllocationType::kYoung,
             Type::OtherObject());
  a.Store(AccessBuilder::ForMap(), map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSStringIteratorString(), string);
  a.Store(AccessBuilder::ForJSStringIteratorIndex(), jsgraph()->SmiConstant(0));
  STATIC_ASSERT(JSIteratorResult::kSize == 5 * kTaggedSize);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateKeyValueArray(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateKeyValueArray, node->opcode());
  Node* key = NodeProperties::GetValueInput(node, 0);
  Node* value = NodeProperties::GetValueInput(node, 1);
  Node* effect = NodeProperties::GetEffectInput(node);

  Node* array_map =
      jsgraph()->Constant(native_context().js_array_packed_elements_map());
  Node* length = jsgraph()->Constant(2);

  AllocationBuilder aa(jsgraph(), effect, graph()->start());
  aa.AllocateArray(2, MakeRef(broker(), factory()->fixed_array_map()));
  aa.Store(AccessBuilder::ForFixedArrayElement(PACKED_ELEMENTS),
           jsgraph()->ZeroConstant(), key);
  aa.Store(AccessBuilder::ForFixedArrayElement(PACKED_ELEMENTS),
           jsgraph()->OneConstant(), value);
  Node* elements = aa.Finish();

  AllocationBuilder a(jsgraph(), elements, graph()->start());
  a.Allocate(JSArray::kHeaderSize);
  a.Store(AccessBuilder::ForMap(), array_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(), elements);
  a.Store(AccessBuilder::ForJSArrayLength(PACKED_ELEMENTS), length);
  STATIC_ASSERT(JSArray::kHeaderSize == 4 * kTaggedSize);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreatePromise(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreatePromise, node->opcode());
  Node* effect = NodeProperties::GetEffectInput(node);

  MapRef promise_map = native_context().promise_function().initial_map();

  AllocationBuilder a(jsgraph(), effect, graph()->start());
  a.Allocate(promise_map.instance_size());
  a.Store(AccessBuilder::ForMap(), promise_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectOffset(JSPromise::kReactionsOrResultOffset),
          jsgraph()->ZeroConstant());
  STATIC_ASSERT(v8::Promise::kPending == 0);
  a.Store(AccessBuilder::ForJSObjectOffset(JSPromise::kFlagsOffset),
          jsgraph()->ZeroConstant());
  STATIC_ASSERT(JSPromise::kHeaderSize == 5 * kTaggedSize);
  for (int offset = JSPromise::kHeaderSize;
       offset < JSPromise::kSizeWithEmbedderFields; offset += kTaggedSize) {
    a.Store(AccessBuilder::ForJSObjectOffset(offset),
            jsgraph()->ZeroConstant());
  }
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateLiteralArrayOrObject(Node* node) {
  DCHECK(node->opcode() == IrOpcode::kJSCreateLiteralArray ||
         node->opcode() == IrOpcode::kJSCreateLiteralObject);
  JSCreateLiteralOpNode n(node);
  CreateLiteralParameters const& p = n.Parameters();
  Effect effect = n.effect();
  Control control = n.control();
  ProcessedFeedback const& feedback =
      broker()->GetFeedbackForArrayOrObjectLiteral(p.feedback());
  if (!feedback.IsInsufficient()) {
    AllocationSiteRef site = feedback.AsLiteral().value();
    if (site.IsFastLiteral()) {
      AllocationType allocation = FLAG_allocation_site_pretenuring
                                      ? site.GetAllocationType()
                                      : AllocationType::kYoung;
      JSObjectRef boilerplate = site.boilerplate().value();
      base::Optional<Node*> maybe_value =
          TryAllocateFastLiteral(effect, control, boilerplate, allocation);
      if (!maybe_value.has_value()) {
        TRACE_BROKER_MISSING(broker(), "bound argument");
        return NoChange();
      }
      if (FLAG_allocation_site_pretenuring) {
        CHECK_EQ(dependencies()->DependOnPretenureMode(site), allocation);
      }
      dependencies()->DependOnElementsKinds(site);
      Node* value = effect = maybe_value.value();
      ReplaceWithValue(node, value, effect, control);
      return Replace(value);
    }
  }

  return NoChange();
}

Reduction JSCreateLowering::ReduceJSCreateEmptyLiteralArray(Node* node) {
  JSCreateEmptyLiteralArrayNode n(node);
  FeedbackParameter const& p = n.Parameters();
  ProcessedFeedback const& feedback =
      broker()->GetFeedbackForArrayOrObjectLiteral(p.feedback());
  if (!feedback.IsInsufficient()) {
    AllocationSiteRef site = feedback.AsLiteral().value();
    DCHECK(!site.PointsToLiteral());
    MapRef initial_map =
        native_context().GetInitialJSArrayMap(site.GetElementsKind());
    AllocationType const allocation =
        dependencies()->DependOnPretenureMode(site);
    dependencies()->DependOnElementsKind(site);
    Node* length = jsgraph()->ZeroConstant();
    DCHECK(!initial_map.IsInobjectSlackTrackingInProgress());
    SlackTrackingPrediction slack_tracking_prediction(
        initial_map, initial_map.instance_size());
    return ReduceNewArray(node, length, 0, initial_map,
                          initial_map.elements_kind(), allocation,
                          slack_tracking_prediction);
  }
  return NoChange();
}

Reduction JSCreateLowering::ReduceJSCreateEmptyLiteralObject(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateEmptyLiteralObject, node->opcode());
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);

  // Retrieve the initial map for the object.
  MapRef map = native_context().object_function().initial_map();
  DCHECK(!map.is_dictionary_map());
  DCHECK(!map.IsInobjectSlackTrackingInProgress());
  Node* js_object_map = jsgraph()->Constant(map);

  // Setup elements and properties.
  Node* elements = jsgraph()->EmptyFixedArrayConstant();

  // Perform the allocation of the actual JSArray object.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(map.instance_size());
  a.Store(AccessBuilder::ForMap(), js_object_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
          jsgraph()->EmptyFixedArrayConstant());
  a.Store(AccessBuilder::ForJSObjectElements(), elements);
  for (int i = 0; i < map.GetInObjectProperties(); i++) {
    a.Store(AccessBuilder::ForJSObjectInObjectProperty(map, i),
            jsgraph()->UndefinedConstant());
  }

  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateLiteralRegExp(Node* node) {
  JSCreateLiteralRegExpNode n(node);
  CreateLiteralParameters const& p = n.Parameters();
  Effect effect = n.effect();
  Control control = n.control();
  ProcessedFeedback const& feedback =
      broker()->GetFeedbackForRegExpLiteral(p.feedback());
  if (!feedback.IsInsufficient()) {
    RegExpBoilerplateDescriptionRef literal =
        feedback.AsRegExpLiteral().value();
    Node* value = effect = AllocateLiteralRegExp(effect, control, literal);
    ReplaceWithValue(node, value, effect, control);
    return Replace(value);
  }
  return NoChange();
}

Reduction JSCreateLowering::ReduceJSGetTemplateObject(Node* node) {
  JSGetTemplateObjectNode n(node);
  GetTemplateObjectParameters const& parameters = n.Parameters();

  const ProcessedFeedback& feedback =
      broker()->GetFeedbackForTemplateObject(parameters.feedback());
  // TODO(v8:7790): Consider not generating JSGetTemplateObject operator
  // in the BytecodeGraphBuilder in the first place, if template_object is not
  // available.
  if (feedback.IsInsufficient()) return NoChange();

  JSArrayRef template_object = feedback.AsTemplateObject().value();
  Node* value = jsgraph()->Constant(template_object);
  ReplaceWithValue(node, value);
  return Replace(value);
}

Reduction JSCreateLowering::ReduceJSCreateFunctionContext(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateFunctionContext, node->opcode());
  const CreateFunctionContextParameters& parameters =
      CreateFunctionContextParametersOf(node->op());
  ScopeInfoRef scope_info = MakeRef(broker(), parameters.scope_info());
  int slot_count = parameters.slot_count();
  ScopeType scope_type = parameters.scope_type();

  // Use inline allocation for function contexts up to a size limit.
  if (slot_count < kFunctionContextAllocationLimit) {
    // JSCreateFunctionContext[slot_count < limit]](fun)
    Node* effect = NodeProperties::GetEffectInput(node);
    Node* control = NodeProperties::GetControlInput(node);
    Node* context = NodeProperties::GetContextInput(node);
    AllocationBuilder a(jsgraph(), effect, control);
    STATIC_ASSERT(Context::MIN_CONTEXT_SLOTS == 2);  // Ensure fully covered.
    int context_length = slot_count + Context::MIN_CONTEXT_SLOTS;
    switch (scope_type) {
      case EVAL_SCOPE:
        a.AllocateContext(context_length, native_context().eval_context_map());
        break;
      case FUNCTION_SCOPE:
        a.AllocateContext(context_length,
                          native_context().function_context_map());
        break;
      default:
        UNREACHABLE();
    }
    a.Store(AccessBuilder::ForContextSlot(Context::SCOPE_INFO_INDEX),
            scope_info);
    a.Store(AccessBuilder::ForContextSlot(Context::PREVIOUS_INDEX), context);
    for (int i = Context::MIN_CONTEXT_SLOTS; i < context_length; ++i) {
      a.Store(AccessBuilder::ForContextSlot(i), jsgraph()->UndefinedConstant());
    }
    RelaxControls(node);
    a.FinishAndChange(node);
    return Changed(node);
  }

  return NoChange();
}

Reduction JSCreateLowering::ReduceJSCreateWithContext(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateWithContext, node->opcode());
  ScopeInfoRef scope_info = MakeRef(broker(), ScopeInfoOf(node->op()));
  Node* extension = NodeProperties::GetValueInput(node, 0);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);
  Node* context = NodeProperties::GetContextInput(node);

  AllocationBuilder a(jsgraph(), effect, control);
  STATIC_ASSERT(Context::MIN_CONTEXT_EXTENDED_SLOTS ==
                3);  // Ensure fully covered.
  a.AllocateContext(Context::MIN_CONTEXT_EXTENDED_SLOTS,
                    native_context().with_context_map());
  a.Store(AccessBuilder::ForContextSlot(Context::SCOPE_INFO_INDEX), scope_info);
  a.Store(AccessBuilder::ForContextSlot(Context::PREVIOUS_INDEX), context);
  a.Store(AccessBuilder::ForContextSlot(Context::EXTENSION_INDEX), extension);
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateCatchContext(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateCatchContext, node->opcode());
  ScopeInfoRef scope_info = MakeRef(broker(), ScopeInfoOf(node->op()));
  Node* exception = NodeProperties::GetValueInput(node, 0);
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);
  Node* context = NodeProperties::GetContextInput(node);

  AllocationBuilder a(jsgraph(), effect, control);
  STATIC_ASSERT(Context::MIN_CONTEXT_SLOTS == 2);  // Ensure fully covered.
  a.AllocateContext(Context::MIN_CONTEXT_SLOTS + 1,
                    native_context().catch_context_map());
  a.Store(AccessBuilder::ForContextSlot(Context::SCOPE_INFO_INDEX), scope_info);
  a.Store(AccessBuilder::ForContextSlot(Context::PREVIOUS_INDEX), context);
  a.Store(AccessBuilder::ForContextSlot(Context::THROWN_OBJECT_INDEX),
          exception);
  RelaxControls(node);
  a.FinishAndChange(node);
  return Changed(node);
}

Reduction JSCreateLowering::ReduceJSCreateBlockContext(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateBlockContext, node->opcode());
  ScopeInfoRef scope_info = MakeRef(broker(), ScopeInfoOf(node->op()));
  int const context_length = scope_info.ContextLength();

  // Use inline allocation for block contexts up to a size limit.
  if (context_length < kBlockContextAllocationLimit) {
    // JSCreateBlockContext[scope[length < limit]](fun)
    Node* effect = NodeProperties::GetEffectInput(node);
    Node* control = NodeProperties::GetControlInput(node);
    Node* context = NodeProperties::GetContextInput(node);

    AllocationBuilder a(jsgraph(), effect, control);
    STATIC_ASSERT(Context::MIN_CONTEXT_SLOTS == 2);  // Ensure fully covered.
    a.AllocateContext(context_length, native_context().block_context_map());
    a.Store(AccessBuilder::ForContextSlot(Context::SCOPE_INFO_INDEX),
            scope_info);
    a.Store(AccessBuilder::ForContextSlot(Context::PREVIOUS_INDEX), context);
    for (int i = Context::MIN_CONTEXT_SLOTS; i < context_length; ++i) {
      a.Store(AccessBuilder::ForContextSlot(i), jsgraph()->UndefinedConstant());
    }
    RelaxControls(node);
    a.FinishAndChange(node);
    return Changed(node);
  }

  return NoChange();
}

namespace {
base::Optional<MapRef> GetObjectCreateMap(JSHeapBroker* broker,
                                          HeapObjectRef prototype) {
  MapRef standard_map =
      broker->target_native_context().object_function().initial_map();
  if (prototype.equals(standard_map.prototype().value())) {
    return standard_map;
  }
  if (prototype.map().oddball_type() == OddballType::kNull) {
    return broker->target_native_context()
        .slow_object_with_null_prototype_map();
  }
  if (prototype.IsJSObject()) {
    return prototype.AsJSObject().GetObjectCreateMap();
  }
  return base::Optional<MapRef>();
}
}  // namespace

Reduction JSCreateLowering::ReduceJSCreateObject(Node* node) {
  DCHECK_EQ(IrOpcode::kJSCreateObject, node->opcode());
  Node* effect = NodeProperties::GetEffectInput(node);
  Node* control = NodeProperties::GetControlInput(node);
  Node* prototype = NodeProperties::GetValueInput(node, 0);
  Type prototype_type = NodeProperties::GetType(prototype);
  if (!prototype_type.IsHeapConstant()) return NoChange();

  HeapObjectRef prototype_const = prototype_type.AsHeapConstant()->Ref();
  auto maybe_instance_map = GetObjectCreateMap(broker(), prototype_const);
  if (!maybe_instance_map) return NoChange();
  MapRef instance_map = maybe_instance_map.value();

  Node* properties = jsgraph()->EmptyFixedArrayConstant();
  if (instance_map.is_dictionary_map()) {
    DCHECK_EQ(prototype_const.map().oddball_type(), OddballType::kNull);
    // Allocate an empty NameDictionary as backing store for the properties.
    MapRef map = MakeRef(broker(), factory()->name_dictionary_map());
    int capacity =
        NameDictionary::ComputeCapacity(NameDictionary::kInitialCapacity);
    DCHECK(base::bits::IsPowerOfTwo(capacity));
    int length = NameDictionary::EntryToIndex(InternalIndex(capacity));
    int size = NameDictionary::SizeFor(length);

    AllocationBuilder a(jsgraph(), effect, control);
    a.Allocate(size, AllocationType::kYoung, Type::Any());
    a.Store(AccessBuilder::ForMap(), map);
    // Initialize FixedArray fields.
    a.Store(AccessBuilder::ForFixedArrayLength(),
            jsgraph()->SmiConstant(length));
    // Initialize HashTable fields.
    a.Store(AccessBuilder::ForHashTableBaseNumberOfElements(),
            jsgraph()->SmiConstant(0));
    a.Store(AccessBuilder::ForHashTableBaseNumberOfDeletedElement(),
            jsgraph()->SmiConstant(0));
    a.Store(AccessBuilder::ForHashTableBaseCapacity(),
            jsgraph()->SmiConstant(capacity));
    // Initialize Dictionary fields.
    a.Store(AccessBuilder::ForDictionaryNextEnumerationIndex(),
            jsgraph()->SmiConstant(PropertyDetails::kInitialIndex));
    a.Store(AccessBuilder::ForDictionaryObjectHashIndex(),
            jsgraph()->SmiConstant(PropertyArray::kNoHashSentinel));
    // Initialize the Properties fields.
    Node* undefined = jsgraph()->UndefinedConstant();
    STATIC_ASSERT(NameDictionary::kElementsStartIndex ==
                  NameDictionary::kObjectHashIndex + 1);
    for (int index = NameDictionary::kElementsStartIndex; index < length;
         index++) {
      a.Store(AccessBuilder::ForFixedArraySlot(index, kNoWriteBarrier),
              undefined);
    }
    properties = effect = a.Finish();
  }

  int const instance_size = instance_map.instance_size();
  if (instance_size > kMaxRegularHeapObjectSize) return NoChange();
  CHECK(!instance_map.IsInobjectSlackTrackingInProgress());

  // Emit code to allocate the JSObject instance for the given
  // {instance_map}.
  AllocationBuilder a(jsgraph(), effect, control);
  a.Allocate(instance_size, AllocationType::kYoung, Type::Any());
  a.Store(AccessBuilder::ForMap(), instance_map);
  a.Store(AccessBuilder::ForJSObjectPropertiesOrHash(), properties);
  a.Store(AccessBuilder::ForJSObjectElements(),
          jsgraph()->EmptyFixedArrayConstant());
  // Initialize Object fields.
  Node* undefined = jsgraph()->UndefinedConstant();
  for (int offset = JSObject::kHeaderSize; offset < instance_size;
       offset += kTaggedSize) {
    a.Store(AccessBuilder::ForJSObjectOffset(offset, kNoWriteBarrier),
            undefined);
  }
  Node* value = effect = a.Finish();

  ReplaceWithValue(node, value, effect, control);
  return Replace(value);
}

// Helper that allocates a FixedArray holding argument values recorded in the
// given {frame_state}. Serves as backing store for JSCreateArguments nodes.
Node* JSCreateLowering::TryAllocateArguments(Node* effect, Node* control,
                                             FrameState frame_state) {
  FrameStateInfo state_info = frame_state.frame_state_info();
  int argument_count = state_info.parameter_count() - 1;  // Minus receiver.
  if (argument_count == 0) return jsgraph()->EmptyFixedArrayConstant();

  // Prepare an iterator over argument values recorded in the frame state.
  Node* const parameters = frame_state.parameters();
  StateValuesAccess parameters_access(parameters);
  auto parameters_it = parameters_access.begin_without_receiver();

  // Actually allocate the backing store.
  MapRef fixed_array_map = MakeRef(broker(), factory()->fixed_array_map());
  AllocationBuilder ab(jsgraph(), effect, control);
  if (!ab.CanAllocateArray(argument_count, fixed_array_map)) {
    return nullptr;
  }
  ab.AllocateArray(argument_count, fixed_array_map);
  for (int i = 0; i < argument_count; ++i, ++parameters_it) {
    DCHECK_NOT_NULL(parameters_it.node());
    ab.Store(AccessBuilder::ForFixedArrayElement(), jsgraph()->Constant(i),
             parameters_it.node());
  }
  return ab.Finish();
}

// Helper that allocates a FixedArray holding argument values recorded in the
// given {frame_state}. Serves as backing store for JSCreateArguments nodes.
Node* JSCreateLowering::TryAllocateRestArguments(Node* effect, Node* control,
                                                 FrameState frame_state,
                                                 int start_index) {
  FrameStateInfo state_info = frame_state.frame_state_info();
  int argument_count = state_info.parameter_count() - 1;  // Minus receiver.
  int num_elements = std::max(0, argument_count - start_index);
  if (num_elements == 0) return jsgraph()->EmptyFixedArrayConstant();

  // Prepare an iterator over argument values recorded in the frame state.
  Node* const parameters = frame_state.parameters();
  StateValuesAccess parameters_access(parameters);
  auto parameters_it =
      parameters_access.begin_without_receiver_and_skip(start_index);

  // Actually allocate the backing store.
  MapRef fixed_array_map = MakeRef(broker(), factory()->fixed_array_map());
  AllocationBuilder ab(jsgraph(), effect, control);
  if (!ab.CanAllocateArray(num_elements, fixed_array_map)) {
    return nullptr;
  }
  ab.AllocateArray(num_elements, fixed_array_map);
  for (int i = 0; i < num_elements; ++i, ++parameters_it) {
    DCHECK_NOT_NULL(parameters_it.node());
    ab.Store(AccessBuilder::ForFixedArrayElement(), jsgraph()->Constant(i),
             parameters_it.node());
  }
  return ab.Finish();
}

// Helper that allocates a FixedArray serving as a parameter map for values
// recorded in the given {frame_state}. Some elements map to slots within the
// given {context}. Serves as backing store for JSCreateArguments nodes.
Node* JSCreateLowering::TryAllocateAliasedArguments(
    Node* effect, Node* control, FrameState frame_state, Node* context,
    const SharedFunctionInfoRef& shared, bool* has_aliased_arguments) {
  FrameStateInfo state_info = frame_state.frame_state_info();
  int argument_count = state_info.parameter_count() - 1;  // Minus receiver.
  if (argument_count == 0) return jsgraph()->EmptyFixedArrayConstant();

  // If there is no aliasing, the arguments object elements are not special in
  // any way, we can just return an unmapped backing store instead.
  int parameter_count = shared.internal_formal_parameter_count();
  if (parameter_count == 0) {
    return TryAllocateArguments(effect, control, frame_state);
  }

  // Calculate number of argument values being aliased/mapped.
  int mapped_count = std::min(argument_count, parameter_count);
  *has_aliased_arguments = true;

  MapRef sloppy_arguments_elements_map =
      MakeRef(broker(), factory()->sloppy_arguments_elements_map());
  if (!AllocationBuilder::CanAllocateSloppyArgumentElements(
          mapped_count, sloppy_arguments_elements_map)) {
    return nullptr;
  }

  MapRef fixed_array_map = MakeRef(broker(), factory()->fixed_array_map());
  if (!AllocationBuilder::CanAllocateArray(argument_count, fixed_array_map)) {
    return nullptr;
  }

  // Prepare an iterator over argument values recorded in the frame state.
  Node* const parameters = frame_state.parameters();
  StateValuesAccess parameters_access(parameters);
  auto parameters_it =
      parameters_access.begin_without_receiver_and_skip(mapped_count);

  // The unmapped argument values recorded in the frame state are stored yet
  // another indirection away and then linked into the parameter map below,
  // whereas mapped argument values are replaced with a hole instead.
  AllocationBuilder ab(jsgraph(), effect, control);
  ab.AllocateArray(argument_count, fixed_array_map);
  for (int i = 0; i < mapped_count; ++i) {
    ab.Store(AccessBuilder::ForFixedArrayElement(), jsgraph()->Constant(i),
             jsgraph()->TheHoleConstant());
  }
  for (int i = mapped_count; i < argument_count; ++i, ++parameters_it) {
    DCHECK_NOT_NULL(parameters_it.node());
    ab.Store(AccessBuilder::ForFixedArrayElement(), jsgraph()->Constant(i),
             parameters_it.node());
  }
  Node* arguments = ab.Finish();

  // Actually allocate the backing store.
  AllocationBuilder a(jsgraph(), arguments, control);
  a.AllocateSloppyArgumentElements(mapped_count, sloppy_arguments_elements_map);
  a.Store(AccessBuilder::ForSloppyArgumentsElementsContext(), context);
  a.Store(AccessBuilder::ForSloppyArgumentsElementsArguments(), arguments);
  for (int i = 0; i < mapped_count; ++i) {
    int idx = shared.context_header_size() + parameter_count - 1 - i;
    a.Store(AccessBuilder::ForSloppyArgumentsElementsMappedEntry(),
            jsgraph()->Constant(i), jsgraph()->Constant(idx));
  }
  return a.Finish();
}

// Helper that allocates a FixedArray serving as a parameter map for values
// unknown at compile-time, the true {arguments_length} and {arguments_frame}
// values can only be determined dynamically at run-time and are provided.
// Serves as backing store for JSCreateArguments nodes.
Node* JSCreateLowering::TryAllocateAliasedArguments(
    Node* effect, Node* control, Node* context, Node* arguments_length,
    const SharedFunctionInfoRef& shared, bool* has_aliased_arguments) {
  // If there is no aliasing, the arguments object elements are not
  // special in any way, we can just return an unmapped backing store.
  int parameter_count = shared.internal_formal_parameter_count();
  if (parameter_count == 0) {
    return graph()->NewNode(
        simplified()->NewArgumentsElements(
            CreateArgumentsType::kUnmappedArguments, parameter_count),
        arguments_length, effect);
  }

  int mapped_count = parameter_count;
  MapRef sloppy_arguments_elements_map =
      MakeRef(broker(), factory()->sloppy_arguments_elements_map());
  if (!AllocationBuilder::CanAllocateSloppyArgumentElements(
          mapped_count, sloppy_arguments_elements_map)) {
    return nullptr;
  }

  // From here on we are going to allocate a mapped (aka. aliased) elements
  // backing store. We do not statically know how many arguments exist, but
  // dynamically selecting the hole for some of the "mapped" elements allows
  // using a static shape for the parameter map.
  *has_aliased_arguments = true;

  // The unmapped argument values are stored yet another indirection away and
  // then linked into the parameter map below, whereas mapped argument values
  // (i.e. the first {mapped_count} elements) are replaced with a hole instead.
  Node* arguments = effect =
      graph()->NewNode(simplified()->NewArgumentsElements(
                           CreateArgumentsType::kMappedArguments, mapped_count),
                       arguments_length, effect);

  // Actually allocate the backing store.
  AllocationBuilder a(jsgraph(), effect, control);
  a.AllocateSloppyArgumentElements(mapped_count, sloppy_arguments_elements_map);
  a.Store(AccessBuilder::ForSloppyArgumentsElementsContext(), context);
  a.Store(AccessBuilder::ForSloppyArgumentsElementsArguments(), arguments);
  for (int i = 0; i < mapped_count; ++i) {
    int idx = shared.context_header_size() + parameter_count - 1 - i;
    Node* value = graph()->NewNode(
        common()->Select(MachineRepresentation::kTagged),
        graph()->NewNode(simplified()->NumberLessThan(), jsgraph()->Constant(i),
                         arguments_length),
        jsgraph()->Constant(idx), jsgraph()->TheHoleConstant());
    a.Store(AccessBuilder::ForSloppyArgumentsElementsMappedEntry(),
            jsgraph()->Constant(i), value);
  }
  return a.Finish();
}

Node* JSCreateLowering::AllocateElements(Node* effect, Node* control,
                                         ElementsKind elements_kind,
                                         int capacity,
                                         AllocationType allocation) {
  DCHECK_LE(1, capacity);
  DCHECK_LE(capacity, JSArray::kInitialMaxFastElementArray);

  Handle<Map> elements_map = IsDoubleElementsKind(elements_kind)
                                 ? factory()->fixed_double_array_map()
                                 : factory()->fixed_array_map();
  ElementAccess access = IsDoubleElementsKind(elements_kind)
                             ? AccessBuilder::ForFixedDoubleArrayElement()
                             : AccessBuilder::ForFixedArrayElement();
  Node* value = jsgraph()->TheHoleConstant();

  // Actually allocate the backing store.
  AllocationBuilder a(jsgraph(), effect, control);
  a.AllocateArray(capacity, MakeRef(broker(), elements_map), allocation);
  for (int i = 0; i < capacity; ++i) {
    Node* index = jsgraph()->Constant(i);
    a.Store(access, index, value);
  }
  return a.Finish();
}

Node* JSCreateLowering::AllocateElements(Node* effect, Node* control,
                                         ElementsKind elements_kind,
                                         std::vector<Node*> const& values,
                                         AllocationType allocation) {
  int const capacity = static_cast<int>(values.size());
  DCHECK_LE(1, capacity);
  DCHECK_LE(capacity, JSArray::kInitialMaxFastElementArray);

  Handle<Map> elements_map = IsDoubleElementsKind(elements_kind)
                                 ? factory()->fixed_double_array_map()
                                 : factory()->fixed_array_map();
  ElementAccess access = IsDoubleElementsKind(elements_kind)
                             ? AccessBuilder::ForFixedDoubleArrayElement()
                             : AccessBuilder::ForFixedArrayElement();

  // Actually allocate the backing store.
  AllocationBuilder a(jsgraph(), effect, control);
  a.AllocateArray(capacity, MakeRef(broker(), elements_map), allocation);
  for (int i = 0; i < capacity; ++i) {
    Node* index = jsgraph()->Constant(i);
    a.Store(access, index, values[i]);
  }
  return a.Finish();
}

base::Optional<Node*> JSCreateLowering::TryAllocateFastLiteral(
    Node* effect, Node* control, JSObjectRef boilerplate,
    AllocationType allocation) {
  // Prevent concurrent migrations of boilerplate objects.
  JSHeapBroker::BoilerplateMigrationGuardIfNeeded boilerplate_access_guard(
      broker());

  // Now that we hold the migration lock, get the current map.
  MapRef boilerplate_map = boilerplate.map();
  base::Optional<MapRef> current_boilerplate_map =
      boilerplate.map_direct_read();

  if (!current_boilerplate_map.has_value() ||
      !current_boilerplate_map.value().equals(boilerplate_map)) {
    return {};
  }

  // Compute the in-object properties to store first (might have effects).
  ZoneVector<std::pair<FieldAccess, Node*>> inobject_fields(zone());
  inobject_fields.reserve(boilerplate_map.GetInObjectProperties());
  int const boilerplate_nof = boilerplate_map.NumberOfOwnDescriptors();
  for (InternalIndex i : InternalIndex::Range(boilerplate_nof)) {
    PropertyDetails const property_details =
        boilerplate_map.GetPropertyDetails(i);
    if (property_details.location() != kField) continue;
    DCHECK_EQ(kData, property_details.kind());
    NameRef property_name = boilerplate_map.GetPropertyKey(i);
    FieldIndex index = boilerplate_map.GetFieldIndexFor(i);
    ConstFieldInfo const_field_info(boilerplate_map.object());
    FieldAccess access = {kTaggedBase,
                          index.offset(),
                          property_name.object(),
                          MaybeHandle<Map>(),
                          Type::Any(),
                          MachineType::AnyTagged(),
                          kFullWriteBarrier,
                          LoadSensitivity::kUnsafe,
                          const_field_info};

    // Note: the use of RawFastPropertyAt (vs. the higher-level
    // GetOwnFastDataProperty) here is necessary, since the underlying value
    // may be `uninitialized`, which the latter explicitly does not support.
    base::Optional<ObjectRef> maybe_boilerplate_value =
        boilerplate.RawFastPropertyAt(index);
    if (!maybe_boilerplate_value.has_value()) return {};

    // Note: We don't need to take a compilation dependency verifying the value
    // of `boilerplate_value`, since boilerplate properties are constant after
    // initialization modulo map migration. We protect against concurrent map
    // migrations via the boilerplate_migration_access lock.
    ObjectRef boilerplate_value = maybe_boilerplate_value.value();

    // Uninitialized fields are marked through the `uninitialized_value`, or in
    // the case of double representation through a HeapNumber containing a
    // special sentinel value. The boilerplate value is not updated to keep up
    // with the map, thus conversions may be necessary. Specifically:
    //
    // - Smi representation: uninitialized is converted to Smi 0.
    // - Not double representation: the special heap number sentinel is
    //   converted to `uninitialized_value`.
    //
    // Note that although we create nodes to write `uninitialized_value` into
    // the object, the field should be overwritten immediately with a real
    // value, and `uninitialized_value` should never be exposed to JS.
    bool is_uninitialized = false;
    if (boilerplate_value.IsHeapObject() &&
        boilerplate_value.AsHeapObject().map().oddball_type() ==
            OddballType::kUninitialized) {
      is_uninitialized = true;
      access.const_field_info = ConstFieldInfo::None();
    } else if (!property_details.representation().IsDouble() &&
               boilerplate_value.IsHeapNumber() &&
               boilerplate_value.AsHeapNumber().value_as_bits() ==
                   kHoleNanInt64) {
      is_uninitialized = true;
      boilerplate_value =
          MakeRef<HeapObject>(broker(), factory()->uninitialized_value());
      access.const_field_info = ConstFieldInfo::None();
    }

    Node* value;
    if (boilerplate_value.IsJSObject()) {
      JSObjectRef boilerplate_object = boilerplate_value.AsJSObject();
      base::Optional<Node*> maybe_value = TryAllocateFastLiteral(
          effect, control, boilerplate_object, allocation);
      if (!maybe_value.has_value()) return {};
      value = effect = maybe_value.value();
    } else if (property_details.representation().IsDouble()) {
      double number = boilerplate_value.AsHeapNumber().value();
      // Allocate a mutable HeapNumber box and store the value into it.
      AllocationBuilder builder(jsgraph(), effect, control);
      builder.Allocate(HeapNumber::kSize, allocation);
      builder.Store(AccessBuilder::ForMap(),
                    MakeRef(broker(), factory()->heap_number_map()));
      builder.Store(AccessBuilder::ForHeapNumberValue(),
                    jsgraph()->Constant(number));
      value = effect = builder.Finish();
    } else if (property_details.representation().IsSmi()) {
      // Ensure that value is stored as smi.
      value = is_uninitialized ? jsgraph()->ZeroConstant()
                               : jsgraph()->Constant(boilerplate_value.AsSmi());
    } else {
      value = jsgraph()->Constant(boilerplate_value);
    }
    inobject_fields.push_back(std::make_pair(access, value));
  }

  // Fill slack at the end of the boilerplate object with filler maps.
  int const boilerplate_length = boilerplate_map.GetInObjectProperties();
  for (int index = static_cast<int>(inobject_fields.size());
       index < boilerplate_length; ++index) {
    DCHECK(!V8_MAP_PACKING_BOOL);
    // TODO(wenyuzhao): Fix incorrect MachineType when V8_MAP_PACKING is
    // enabled.
    FieldAccess access =
        AccessBuilder::ForJSObjectInObjectProperty(boilerplate_map, index);
    Node* value = jsgraph()->HeapConstant(factory()->one_pointer_filler_map());
    inobject_fields.push_back(std::make_pair(access, value));
  }

  // Setup the elements backing store.
  base::Optional<Node*> maybe_elements =
      TryAllocateFastLiteralElements(effect, control, boilerplate, allocation);
  if (!maybe_elements.has_value()) return {};
  Node* elements = maybe_elements.value();
  if (elements->op()->EffectOutputCount() > 0) effect = elements;

  // Actually allocate and initialize the object.
  AllocationBuilder builder(jsgraph(), effect, control);
  builder.Allocate(boilerplate_map.instance_size(), allocation,
                   Type::For(boilerplate_map));
  builder.Store(AccessBuilder::ForMap(), boilerplate_map);
  builder.Store(AccessBuilder::ForJSObjectPropertiesOrHashKnownPointer(),
                jsgraph()->EmptyFixedArrayConstant());
  builder.Store(AccessBuilder::ForJSObjectElements(), elements);
  if (boilerplate.IsJSArray()) {
    JSArrayRef boilerplate_array = boilerplate.AsJSArray();
    builder.Store(AccessBuilder::ForJSArrayLength(
                      boilerplate_array.map().elements_kind()),
                  boilerplate_array.GetBoilerplateLength());
  }
  for (auto const& inobject_field : inobject_fields) {
    builder.Store(inobject_field.first, inobject_field.second);
  }
  return builder.Finish();
}

base::Optional<Node*> JSCreateLowering::TryAllocateFastLiteralElements(
    Node* effect, Node* control, JSObjectRef boilerplate,
    AllocationType allocation) {
  base::Optional<FixedArrayBaseRef> maybe_boilerplate_elements =
      boilerplate.elements(kRelaxedLoad);
  if (!maybe_boilerplate_elements.has_value()) return {};
  FixedArrayBaseRef boilerplate_elements = maybe_boilerplate_elements.value();

  // Empty or copy-on-write elements just store a constant.
  int const elements_length = boilerplate_elements.length();
  MapRef elements_map = boilerplate_elements.map();
  if (boilerplate_elements.length() == 0 || elements_map.IsFixedCowArrayMap()) {
    if (allocation == AllocationType::kOld &&
        !boilerplate.IsElementsTenured(boilerplate_elements)) {
      return {};
    }
    return jsgraph()->HeapConstant(boilerplate_elements.object());
  }

  // Compute the elements to store first (might have effects).
  ZoneVector<Node*> elements_values(elements_length, zone());
  if (elements_map.instance_type() == FIXED_DOUBLE_ARRAY_TYPE) {
    FixedDoubleArrayRef elements = boilerplate_elements.AsFixedDoubleArray();
    for (int i = 0; i < elements_length; ++i) {
      Float64 value = elements.GetFromImmutableFixedDoubleArray(i);
      if (value.is_hole_nan()) {
        elements_values[i] = jsgraph()->TheHoleConstant();
      } else {
        elements_values[i] = jsgraph()->Constant(value.get_scalar());
      }
    }
  } else {
    FixedArrayRef elements = boilerplate_elements.AsFixedArray();
    for (int i = 0; i < elements_length; ++i) {
      base::Optional<ObjectRef> maybe_element_value = elements.TryGet(i);
      if (!maybe_element_value.has_value()) return {};
      ObjectRef element_value = maybe_element_value.value();
      if (element_value.IsJSObject()) {
        base::Optional<Node*> maybe_value = TryAllocateFastLiteral(
            effect, control, element_value.AsJSObject(), allocation);
        if (!maybe_value.has_value()) return {};
        elements_values[i] = effect = maybe_value.value();
      } else {
        elements_values[i] = jsgraph()->Constant(element_value);
      }
    }
  }

  // Allocate the backing store array and store the elements.
  AllocationBuilder ab(jsgraph(), effect, control);
  CHECK(ab.CanAllocateArray(elements_length, elements_map, allocation));
  ab.AllocateArray(elements_length, elements_map, allocation);
  ElementAccess const access =
      (elements_map.instance_type() == FIXED_DOUBLE_ARRAY_TYPE)
          ? AccessBuilder::ForFixedDoubleArrayElement()
          : AccessBuilder::ForFixedArrayElement();
  for (int i = 0; i < elements_length; ++i) {
    ab.Store(access, jsgraph()->Constant(i), elements_values[i]);
  }
  return ab.Finish();
}

Node* JSCreateLowering::AllocateLiteralRegExp(
    Node* effect, Node* control, RegExpBoilerplateDescriptionRef boilerplate) {
  MapRef initial_map = native_context().regexp_function().initial_map();

  // Sanity check that JSRegExp object layout hasn't changed.
  STATIC_ASSERT(JSRegExp::kDataOffset == JSObject::kHeaderSize);
  STATIC_ASSERT(JSRegExp::kSourceOffset == JSRegExp::kDataOffset + kTaggedSize);
  STATIC_ASSERT(JSRegExp::kFlagsOffset ==
                JSRegExp::kSourceOffset + kTaggedSize);
  STATIC_ASSERT(JSRegExp::kHeaderSize == JSRegExp::kFlagsOffset + kTaggedSize);
  STATIC_ASSERT(JSRegExp::kLastIndexOffset == JSRegExp::kHeaderSize);
  DCHECK_EQ(JSRegExp::Size(), JSRegExp::kLastIndexOffset + kTaggedSize);

  AllocationBuilder builder(jsgraph(), effect, control);
  builder.Allocate(JSRegExp::Size(), AllocationType::kYoung,
                   Type::For(initial_map));
  builder.Store(AccessBuilder::ForMap(), initial_map);
  builder.Store(AccessBuilder::ForJSObjectPropertiesOrHash(),
                jsgraph()->EmptyFixedArrayConstant());
  builder.Store(AccessBuilder::ForJSObjectElements(),
                jsgraph()->EmptyFixedArrayConstant());

  builder.Store(AccessBuilder::ForJSRegExpData(), boilerplate.data());
  builder.Store(AccessBuilder::ForJSRegExpSource(), boilerplate.source());
  builder.Store(AccessBuilder::ForJSRegExpFlags(),
                jsgraph()->SmiConstant(boilerplate.flags()));
  builder.Store(AccessBuilder::ForJSRegExpLastIndex(),
                jsgraph()->SmiConstant(JSRegExp::kInitialLastIndexValue));

  return builder.Finish();
}

Factory* JSCreateLowering::factory() const {
  return jsgraph()->isolate()->factory();
}

Graph* JSCreateLowering::graph() const { return jsgraph()->graph(); }

CommonOperatorBuilder* JSCreateLowering::common() const {
  return jsgraph()->common();
}

SimplifiedOperatorBuilder* JSCreateLowering::simplified() const {
  return jsgraph()->simplified();
}

NativeContextRef JSCreateLowering::native_context() const {
  return broker()->target_native_context();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8

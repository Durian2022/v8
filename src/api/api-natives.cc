// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api/api-natives.h"

#include "src/api/api-inl.h"
#include "src/common/message-template.h"
#include "src/execution/isolate-inl.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/lookup.h"
#include "src/objects/property-cell.h"
#include "src/objects/templates.h"

namespace v8 {
namespace internal {

namespace {

class V8_NODISCARD InvokeScope {
 public:
  explicit InvokeScope(Isolate* isolate)
      : isolate_(isolate), save_context_(isolate) {}
  ~InvokeScope() {
    bool has_exception = isolate_->has_pending_exception();
    if (has_exception) {
      isolate_->ReportPendingMessages();
    } else {
      isolate_->clear_pending_message();
    }
  }

 private:
  Isolate* isolate_;
  SaveContext save_context_;
};

MaybeHandle<JSObject> InstantiateObject(Isolate* isolate,
                                        Handle<ObjectTemplateInfo> data,
                                        Handle<JSReceiver> new_target,
                                        bool is_prototype);

MaybeHandle<JSFunction> InstantiateFunction(
    Isolate* isolate, Handle<NativeContext> native_context,
    Handle<FunctionTemplateInfo> data,
    MaybeHandle<Name> maybe_name = MaybeHandle<Name>());

MaybeHandle<JSFunction> InstantiateFunction(
    Isolate* isolate, Handle<FunctionTemplateInfo> data,
    MaybeHandle<Name> maybe_name = MaybeHandle<Name>()) {
  return InstantiateFunction(isolate, isolate->native_context(), data,
                             maybe_name);
}

MaybeHandle<Object> Instantiate(
    Isolate* isolate, Handle<Object> data,
    MaybeHandle<Name> maybe_name = MaybeHandle<Name>()) {
  if (data->IsFunctionTemplateInfo()) {
    return InstantiateFunction(
        isolate, Handle<FunctionTemplateInfo>::cast(data), maybe_name);
  } else if (data->IsObjectTemplateInfo()) {
    return InstantiateObject(isolate, Handle<ObjectTemplateInfo>::cast(data),
                             Handle<JSReceiver>(), false);
  } else {
    return data;
  }
}

MaybeHandle<Object> DefineAccessorProperty(Isolate* isolate,
                                           Handle<JSObject> object,
                                           Handle<Name> name,
                                           Handle<Object> getter,
                                           Handle<Object> setter,
                                           PropertyAttributes attributes) {
  DCHECK(!getter->IsFunctionTemplateInfo() ||
         FunctionTemplateInfo::cast(*getter).should_cache());
  DCHECK(!setter->IsFunctionTemplateInfo() ||
         FunctionTemplateInfo::cast(*setter).should_cache());
  if (getter->IsFunctionTemplateInfo() &&
      FunctionTemplateInfo::cast(*getter).BreakAtEntry()) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, getter,
        InstantiateFunction(isolate,
                            Handle<FunctionTemplateInfo>::cast(getter)),
        Object);
  }
  if (setter->IsFunctionTemplateInfo() &&
      FunctionTemplateInfo::cast(*setter).BreakAtEntry()) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, setter,
        InstantiateFunction(isolate,
                            Handle<FunctionTemplateInfo>::cast(setter)),
        Object);
  }
  RETURN_ON_EXCEPTION(
      isolate,
      JSObject::DefineAccessor(object, name, getter, setter, attributes),
      Object);
  return object;
}

MaybeHandle<Object> DefineDataProperty(Isolate* isolate,
                                       Handle<JSObject> object,
                                       Handle<Name> name,
                                       Handle<Object> prop_data,
                                       PropertyAttributes attributes) {
  Handle<Object> value;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, value,
                             Instantiate(isolate, prop_data, name), Object);

  LookupIterator::Key key(isolate, name);
  LookupIterator it(isolate, object, key, LookupIterator::OWN_SKIP_INTERCEPTOR);

#ifdef DEBUG
  Maybe<PropertyAttributes> maybe = JSReceiver::GetPropertyAttributes(&it);
  DCHECK(maybe.IsJust());
  if (it.IsFound()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kDuplicateTemplateProperty, name),
        Object);
  }
#endif

  MAYBE_RETURN_NULL(Object::AddDataProperty(&it, value, attributes,
                                            Just(ShouldThrow::kThrowOnError),
                                            StoreOrigin::kNamed));
  return value;
}

void DisableAccessChecks(Isolate* isolate, Handle<JSObject> object) {
  Handle<Map> old_map(object->map(), isolate);
  // Copy map so it won't interfere constructor's initial map.
  Handle<Map> new_map = Map::Copy(isolate, old_map, "DisableAccessChecks");
  new_map->set_is_access_check_needed(false);
  JSObject::MigrateToMap(isolate, Handle<JSObject>::cast(object), new_map);
}

void EnableAccessChecks(Isolate* isolate, Handle<JSObject> object) {
  Handle<Map> old_map(object->map(), isolate);
  // Copy map so it won't interfere constructor's initial map.
  Handle<Map> new_map = Map::Copy(isolate, old_map, "EnableAccessChecks");
  new_map->set_is_access_check_needed(true);
  new_map->set_may_have_interesting_symbols(true);
  JSObject::MigrateToMap(isolate, object, new_map);
}

class V8_NODISCARD AccessCheckDisableScope {
 public:
  AccessCheckDisableScope(Isolate* isolate, Handle<JSObject> obj)
      : isolate_(isolate),
        disabled_(obj->map().is_access_check_needed()),
        obj_(obj) {
    if (disabled_) {
      DisableAccessChecks(isolate_, obj_);
    }
  }
  ~AccessCheckDisableScope() {
    if (disabled_) {
      EnableAccessChecks(isolate_, obj_);
    }
  }

 private:
  Isolate* isolate_;
  const bool disabled_;
  Handle<JSObject> obj_;
};

Object GetIntrinsic(Isolate* isolate, v8::Intrinsic intrinsic) {
  Handle<Context> native_context = isolate->native_context();
  DCHECK(!native_context.is_null());
  switch (intrinsic) {
#define GET_INTRINSIC_VALUE(name, iname) \
  case v8::k##name:                      \
    return native_context->iname();
    V8_INTRINSICS_LIST(GET_INTRINSIC_VALUE)
#undef GET_INTRINSIC_VALUE
  }
  return Object();
}

template <typename TemplateInfoT>
MaybeHandle<JSObject> ConfigureInstance(Isolate* isolate, Handle<JSObject> obj,
                                        Handle<TemplateInfoT> data) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kConfigureInstance);
  HandleScope scope(isolate);
  // Disable access checks while instantiating the object.
  AccessCheckDisableScope access_check_scope(isolate, obj);

  // Walk the inheritance chain and copy all accessors to current object.
  int max_number_of_properties = 0;
  TemplateInfoT info = *data;
  while (!info.is_null()) {
    Object props = info.property_accessors();
    if (!props.IsUndefined(isolate)) {
      max_number_of_properties += TemplateList::cast(props).length();
    }
    info = info.GetParent(isolate);
  }

  if (max_number_of_properties > 0) {
    int valid_descriptors = 0;
    // Use a temporary FixedArray to accumulate unique accessors.
    Handle<FixedArray> array =
        isolate->factory()->NewFixedArray(max_number_of_properties);

    for (Handle<TemplateInfoT> temp(*data, isolate); !temp->is_null();
         temp = handle(temp->GetParent(isolate), isolate)) {
      // Accumulate accessors.
      Object maybe_properties = temp->property_accessors();
      if (!maybe_properties.IsUndefined(isolate)) {
        valid_descriptors = AccessorInfo::AppendUnique(
            isolate, handle(maybe_properties, isolate), array,
            valid_descriptors);
      }
    }

    // Install accumulated accessors.
    for (int i = 0; i < valid_descriptors; i++) {
      Handle<AccessorInfo> accessor(AccessorInfo::cast(array->get(i)), isolate);
      Handle<Name> name(Name::cast(accessor->name()), isolate);
      JSObject::SetAccessor(obj, name, accessor,
                            accessor->initial_property_attributes())
          .Assert();
    }
  }

  Object maybe_property_list = data->property_list();
  if (maybe_property_list.IsUndefined(isolate)) return obj;
  Handle<TemplateList> properties(TemplateList::cast(maybe_property_list),
                                  isolate);
  if (properties->length() == 0) return obj;

  int i = 0;
  for (int c = 0; c < data->number_of_properties(); c++) {
    auto name = handle(Name::cast(properties->get(i++)), isolate);
    Object bit = properties->get(i++);
    if (bit.IsSmi()) {
      PropertyDetails details(Smi::cast(bit));
      PropertyAttributes attributes = details.attributes();
      PropertyKind kind = details.kind();

      if (kind == kData) {
        auto prop_data = handle(properties->get(i++), isolate);
        RETURN_ON_EXCEPTION(
            isolate,
            DefineDataProperty(isolate, obj, name, prop_data, attributes),
            JSObject);
      } else {
        auto getter = handle(properties->get(i++), isolate);
        auto setter = handle(properties->get(i++), isolate);
        RETURN_ON_EXCEPTION(isolate,
                            DefineAccessorProperty(isolate, obj, name, getter,
                                                   setter, attributes),
                            JSObject);
      }
    } else {
      // Intrinsic data property --- Get appropriate value from the current
      // context.
      PropertyDetails details(Smi::cast(properties->get(i++)));
      PropertyAttributes attributes = details.attributes();
      DCHECK_EQ(kData, details.kind());

      v8::Intrinsic intrinsic =
          static_cast<v8::Intrinsic>(Smi::ToInt(properties->get(i++)));
      auto prop_data = handle(GetIntrinsic(isolate, intrinsic), isolate);

      RETURN_ON_EXCEPTION(
          isolate,
          DefineDataProperty(isolate, obj, name, prop_data, attributes),
          JSObject);
    }
  }
  return obj;
}

// Whether or not to cache every instance: when we materialize a getter or
// setter from an lazy AccessorPair, we rely on this cache to be able to always
// return the same getter or setter. However, objects will be cloned anyways,
// so it's not observable if we didn't cache an instance. Furthermore, a badly
// behaved embedder might create an unlimited number of objects, so we limit
// the cache for those cases.
enum class CachingMode { kLimited, kUnlimited };

MaybeHandle<JSObject> ProbeInstantiationsCache(
    Isolate* isolate, Handle<NativeContext> native_context, int serial_number,
    CachingMode caching_mode) {
  DCHECK_NE(serial_number, TemplateInfo::kDoNotCache);
  if (serial_number == TemplateInfo::kUncached) {
    return {};
  }

  if (serial_number < TemplateInfo::kFastTemplateInstantiationsCacheSize) {
    FixedArray fast_cache =
        native_context->fast_template_instantiations_cache();
    Handle<Object> object{fast_cache.get(serial_number), isolate};
    if (object->IsTheHole(isolate)) return {};
    return Handle<JSObject>::cast(object);
  }
  if (caching_mode == CachingMode::kUnlimited ||
      (serial_number < TemplateInfo::kSlowTemplateInstantiationsCacheSize)) {
    SimpleNumberDictionary slow_cache =
        native_context->slow_template_instantiations_cache();
    InternalIndex entry = slow_cache.FindEntry(isolate, serial_number);
    if (entry.is_found()) {
      return handle(JSObject::cast(slow_cache.ValueAt(entry)), isolate);
    }
  }
  return {};
}

void CacheTemplateInstantiation(Isolate* isolate,
                                Handle<NativeContext> native_context,
                                Handle<TemplateInfo> data,
                                CachingMode caching_mode,
                                Handle<JSObject> object) {
  DCHECK_NE(TemplateInfo::kDoNotCache, data->serial_number());

  int serial_number = data->serial_number();
  if (serial_number == TemplateInfo::kUncached) {
    serial_number = isolate->heap()->GetNextTemplateSerialNumber();
  }

  if (serial_number < TemplateInfo::kFastTemplateInstantiationsCacheSize) {
    Handle<FixedArray> fast_cache =
        handle(native_context->fast_template_instantiations_cache(), isolate);
    Handle<FixedArray> new_cache =
        FixedArray::SetAndGrow(isolate, fast_cache, serial_number, object);
    if (*new_cache != *fast_cache) {
      native_context->set_fast_template_instantiations_cache(*new_cache);
    }
    data->set_serial_number(serial_number);
  } else if (caching_mode == CachingMode::kUnlimited ||
             (serial_number <
              TemplateInfo::kSlowTemplateInstantiationsCacheSize)) {
    Handle<SimpleNumberDictionary> cache =
        handle(native_context->slow_template_instantiations_cache(), isolate);
    auto new_cache =
        SimpleNumberDictionary::Set(isolate, cache, serial_number, object);
    if (*new_cache != *cache) {
      native_context->set_slow_template_instantiations_cache(*new_cache);
    }
    data->set_serial_number(serial_number);
  } else {
    // we've overflowed the cache limit, no more caching
    data->set_serial_number(TemplateInfo::kDoNotCache);
  }
}

void UncacheTemplateInstantiation(Isolate* isolate,
                                  Handle<NativeContext> native_context,
                                  Handle<TemplateInfo> data,
                                  CachingMode caching_mode) {
  int serial_number = data->serial_number();
  if (serial_number < 0) return;

  if (serial_number < TemplateInfo::kFastTemplateInstantiationsCacheSize) {
    FixedArray fast_cache =
        native_context->fast_template_instantiations_cache();
    DCHECK(!fast_cache.get(serial_number).IsUndefined(isolate));
    fast_cache.set_undefined(serial_number);
    data->set_serial_number(TemplateInfo::kUncached);
  } else if (caching_mode == CachingMode::kUnlimited ||
             (serial_number <
              TemplateInfo::kSlowTemplateInstantiationsCacheSize)) {
    Handle<SimpleNumberDictionary> cache =
        handle(native_context->slow_template_instantiations_cache(), isolate);
    InternalIndex entry = cache->FindEntry(isolate, serial_number);
    DCHECK(entry.is_found());
    cache = SimpleNumberDictionary::DeleteEntry(isolate, cache, entry);
    native_context->set_slow_template_instantiations_cache(*cache);
    data->set_serial_number(TemplateInfo::kUncached);
  }
}

bool IsSimpleInstantiation(Isolate* isolate, ObjectTemplateInfo info,
                           JSReceiver new_target) {
  DisallowGarbageCollection no_gc;

  if (!new_target.IsJSFunction()) return false;
  JSFunction fun = JSFunction::cast(new_target);
  if (fun.shared().function_data(kAcquireLoad) != info.constructor())
    return false;
  if (info.immutable_proto()) return false;
  return fun.context().native_context() == isolate->raw_native_context();
}

MaybeHandle<JSObject> InstantiateObject(Isolate* isolate,
                                        Handle<ObjectTemplateInfo> info,
                                        Handle<JSReceiver> new_target,
                                        bool is_prototype) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kInstantiateObject);
  Handle<JSFunction> constructor;
  bool should_cache = info->should_cache();
  if (!new_target.is_null()) {
    if (IsSimpleInstantiation(isolate, *info, *new_target)) {
      constructor = Handle<JSFunction>::cast(new_target);
    } else {
      // Disable caching for subclass instantiation.
      should_cache = false;
    }
  }
  // Fast path.
  Handle<JSObject> result;
  if (should_cache && info->is_cached()) {
    if (ProbeInstantiationsCache(isolate, isolate->native_context(),
                                 info->serial_number(), CachingMode::kLimited)
            .ToHandle(&result)) {
      return isolate->factory()->CopyJSObject(result);
    }
  }

  if (constructor.is_null()) {
    Object maybe_constructor_info = info->constructor();
    if (maybe_constructor_info.IsUndefined(isolate)) {
      constructor = isolate->object_function();
    } else {
      // Enter a new scope.  Recursion could otherwise create a lot of handles.
      HandleScope scope(isolate);
      Handle<FunctionTemplateInfo> cons_templ(
          FunctionTemplateInfo::cast(maybe_constructor_info), isolate);
      Handle<JSFunction> tmp_constructor;
      ASSIGN_RETURN_ON_EXCEPTION(isolate, tmp_constructor,
                                 InstantiateFunction(isolate, cons_templ),
                                 JSObject);
      constructor = scope.CloseAndEscape(tmp_constructor);
    }

    if (new_target.is_null()) new_target = constructor;
  }

  Handle<JSObject> object;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, object,
      JSObject::New(constructor, new_target, Handle<AllocationSite>::null()),
      JSObject);

  if (is_prototype) JSObject::OptimizeAsPrototype(object);

  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, result, ConfigureInstance(isolate, object, info), JSObject);
  if (info->immutable_proto()) {
    JSObject::SetImmutableProto(object);
  }
  if (!is_prototype) {
    // Keep prototypes in slow-mode. Let them be lazily turned fast later on.
    // TODO(dcarney): is this necessary?
    JSObject::MigrateSlowToFast(result, 0, "ApiNatives::InstantiateObject");
    // Don't cache prototypes.
    if (should_cache) {
      CacheTemplateInstantiation(isolate, isolate->native_context(), info,
                                 CachingMode::kLimited, result);
      result = isolate->factory()->CopyJSObject(result);
    }
  }

  return result;
}

namespace {
MaybeHandle<Object> GetInstancePrototype(Isolate* isolate,
                                         Handle<Object> function_template) {
  // Enter a new scope.  Recursion could otherwise create a lot of handles.
  HandleScope scope(isolate);
  Handle<JSFunction> parent_instance;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, parent_instance,
      InstantiateFunction(
          isolate, Handle<FunctionTemplateInfo>::cast(function_template)),
      JSFunction);
  Handle<Object> instance_prototype;
  // TODO(cbruni): decide what to do here.
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, instance_prototype,
      JSObject::GetProperty(isolate, parent_instance,
                            isolate->factory()->prototype_string()),
      JSFunction);
  return scope.CloseAndEscape(instance_prototype);
}
}  // namespace

MaybeHandle<JSFunction> InstantiateFunction(
    Isolate* isolate, Handle<NativeContext> native_context,
    Handle<FunctionTemplateInfo> data, MaybeHandle<Name> maybe_name) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kInstantiateFunction);
  bool should_cache = data->should_cache();
  if (should_cache && data->is_cached()) {
    Handle<JSObject> result;
    if (ProbeInstantiationsCache(isolate, native_context, data->serial_number(),
                                 CachingMode::kUnlimited)
            .ToHandle(&result)) {
      return Handle<JSFunction>::cast(result);
    }
  }
  Handle<Object> prototype;
  if (!data->remove_prototype()) {
    Handle<Object> prototype_templ(data->GetPrototypeTemplate(), isolate);
    if (prototype_templ->IsUndefined(isolate)) {
      Handle<Object> protoype_provider_templ(
          data->GetPrototypeProviderTemplate(), isolate);
      if (protoype_provider_templ->IsUndefined(isolate)) {
        prototype = isolate->factory()->NewJSObject(isolate->object_function());
      } else {
        ASSIGN_RETURN_ON_EXCEPTION(
            isolate, prototype,
            GetInstancePrototype(isolate, protoype_provider_templ), JSFunction);
      }
    } else {
      ASSIGN_RETURN_ON_EXCEPTION(
          isolate, prototype,
          InstantiateObject(isolate,
                            Handle<ObjectTemplateInfo>::cast(prototype_templ),
                            Handle<JSReceiver>(), true),
          JSFunction);
    }
    Handle<Object> parent(data->GetParentTemplate(), isolate);
    if (!parent->IsUndefined(isolate)) {
      Handle<Object> parent_prototype;
      ASSIGN_RETURN_ON_EXCEPTION(isolate, parent_prototype,
                                 GetInstancePrototype(isolate, parent),
                                 JSFunction);
      CHECK(parent_prototype->IsHeapObject());
      JSObject::ForceSetPrototype(isolate, Handle<JSObject>::cast(prototype),
                                  Handle<HeapObject>::cast(parent_prototype));
    }
  }
  InstanceType function_type =
      (!data->needs_access_check() &&
       data->GetNamedPropertyHandler().IsUndefined(isolate) &&
       data->GetIndexedPropertyHandler().IsUndefined(isolate))
          ? JS_API_OBJECT_TYPE
          : JS_SPECIAL_API_OBJECT_TYPE;

  Handle<JSFunction> function = ApiNatives::CreateApiFunction(
      isolate, native_context, data, prototype, function_type, maybe_name);
  if (should_cache) {
    // Cache the function.
    CacheTemplateInstantiation(isolate, native_context, data,
                               CachingMode::kUnlimited, function);
  }
  MaybeHandle<JSObject> result = ConfigureInstance(isolate, function, data);
  if (result.is_null()) {
    // Uncache on error.
    UncacheTemplateInstantiation(isolate, native_context, data,
                                 CachingMode::kUnlimited);
    return MaybeHandle<JSFunction>();
  }
  data->set_published(true);
  return function;
}

void AddPropertyToPropertyList(Isolate* isolate, Handle<TemplateInfo> templ,
                               int length, Handle<Object>* data) {
  Object maybe_list = templ->property_list();
  Handle<TemplateList> list;
  if (maybe_list.IsUndefined(isolate)) {
    list = TemplateList::New(isolate, length);
  } else {
    list = handle(TemplateList::cast(maybe_list), isolate);
  }
  templ->set_number_of_properties(templ->number_of_properties() + 1);
  for (int i = 0; i < length; i++) {
    Handle<Object> value =
        data[i].is_null()
            ? Handle<Object>::cast(isolate->factory()->undefined_value())
            : data[i];
    list = TemplateList::Add(isolate, list, value);
  }
  templ->set_property_list(*list);
}

}  // namespace

MaybeHandle<JSFunction> ApiNatives::InstantiateFunction(
    Isolate* isolate, Handle<NativeContext> native_context,
    Handle<FunctionTemplateInfo> data, MaybeHandle<Name> maybe_name) {
  InvokeScope invoke_scope(isolate);
  return ::v8::internal::InstantiateFunction(isolate, native_context, data,
                                             maybe_name);
}

MaybeHandle<JSFunction> ApiNatives::InstantiateFunction(
    Handle<FunctionTemplateInfo> data, MaybeHandle<Name> maybe_name) {
  Isolate* isolate = data->GetIsolate();
  InvokeScope invoke_scope(isolate);
  return ::v8::internal::InstantiateFunction(isolate, data, maybe_name);
}

MaybeHandle<JSObject> ApiNatives::InstantiateObject(
    Isolate* isolate, Handle<ObjectTemplateInfo> data,
    Handle<JSReceiver> new_target) {
  InvokeScope invoke_scope(isolate);
  return ::v8::internal::InstantiateObject(isolate, data, new_target, false);
}

MaybeHandle<JSObject> ApiNatives::InstantiateRemoteObject(
    Handle<ObjectTemplateInfo> data) {
  Isolate* isolate = data->GetIsolate();
  InvokeScope invoke_scope(isolate);

  Handle<FunctionTemplateInfo> constructor(
      FunctionTemplateInfo::cast(data->constructor()), isolate);
  Handle<Map> object_map = isolate->factory()->NewMap(
      JS_SPECIAL_API_OBJECT_TYPE,
      JSObject::kHeaderSize +
          data->embedder_field_count() * kEmbedderDataSlotSize,
      TERMINAL_FAST_ELEMENTS_KIND);
  object_map->SetConstructor(*constructor);
  object_map->set_is_access_check_needed(true);
  object_map->set_may_have_interesting_symbols(true);

  Handle<JSObject> object = isolate->factory()->NewJSObjectFromMap(object_map);
  JSObject::ForceSetPrototype(isolate, object,
                              isolate->factory()->null_value());

  return object;
}

void ApiNatives::AddDataProperty(Isolate* isolate, Handle<TemplateInfo> info,
                                 Handle<Name> name, Handle<Object> value,
                                 PropertyAttributes attributes) {
  PropertyDetails details(kData, attributes, PropertyConstness::kMutable);
  auto details_handle = handle(details.AsSmi(), isolate);
  Handle<Object> data[] = {name, details_handle, value};
  AddPropertyToPropertyList(isolate, info, arraysize(data), data);
}

void ApiNatives::AddDataProperty(Isolate* isolate, Handle<TemplateInfo> info,
                                 Handle<Name> name, v8::Intrinsic intrinsic,
                                 PropertyAttributes attributes) {
  auto value = handle(Smi::FromInt(intrinsic), isolate);
  auto intrinsic_marker = isolate->factory()->true_value();
  PropertyDetails details(kData, attributes, PropertyConstness::kMutable);
  auto details_handle = handle(details.AsSmi(), isolate);
  Handle<Object> data[] = {name, intrinsic_marker, details_handle, value};
  AddPropertyToPropertyList(isolate, info, arraysize(data), data);
}

void ApiNatives::AddAccessorProperty(Isolate* isolate,
                                     Handle<TemplateInfo> info,
                                     Handle<Name> name,
                                     Handle<FunctionTemplateInfo> getter,
                                     Handle<FunctionTemplateInfo> setter,
                                     PropertyAttributes attributes) {
  if (!getter.is_null()) getter->set_published(true);
  if (!setter.is_null()) setter->set_published(true);
  PropertyDetails details(kAccessor, attributes, PropertyConstness::kMutable);
  auto details_handle = handle(details.AsSmi(), isolate);
  Handle<Object> data[] = {name, details_handle, getter, setter};
  AddPropertyToPropertyList(isolate, info, arraysize(data), data);
}

void ApiNatives::AddNativeDataProperty(Isolate* isolate,
                                       Handle<TemplateInfo> info,
                                       Handle<AccessorInfo> property) {
  Object maybe_list = info->property_accessors();
  Handle<TemplateList> list;
  if (maybe_list.IsUndefined(isolate)) {
    list = TemplateList::New(isolate, 1);
  } else {
    list = handle(TemplateList::cast(maybe_list), isolate);
  }
  list = TemplateList::Add(isolate, list, property);
  info->set_property_accessors(*list);
}

Handle<JSFunction> ApiNatives::CreateApiFunction(
    Isolate* isolate, Handle<NativeContext> native_context,
    Handle<FunctionTemplateInfo> obj, Handle<Object> prototype,
    InstanceType type, MaybeHandle<Name> maybe_name) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kCreateApiFunction);
  Handle<SharedFunctionInfo> shared =
      FunctionTemplateInfo::GetOrCreateSharedFunctionInfo(isolate, obj,
                                                          maybe_name);
  // To simplify things, API functions always have shared name.
  DCHECK(shared->HasSharedName());

  Handle<JSFunction> result =
      Factory::JSFunctionBuilder{isolate, shared, native_context}.Build();

  if (obj->remove_prototype()) {
    DCHECK(prototype.is_null());
    DCHECK(result->shared().IsApiFunction());
    DCHECK(!result->IsConstructor());
    DCHECK(!result->has_prototype_slot());
    return result;
  }

  // Down from here is only valid for API functions that can be used as a
  // constructor (don't set the "remove prototype" flag).
  DCHECK(result->has_prototype_slot());

  if (obj->read_only_prototype()) {
    result->set_map(*isolate->sloppy_function_with_readonly_prototype_map());
  }

  if (prototype->IsTheHole(isolate)) {
    prototype = isolate->factory()->NewFunctionPrototype(result);
  } else if (obj->GetPrototypeProviderTemplate().IsUndefined(isolate)) {
    JSObject::AddProperty(isolate, Handle<JSObject>::cast(prototype),
                          isolate->factory()->constructor_string(), result,
                          DONT_ENUM);
  }

  int embedder_field_count = 0;
  bool immutable_proto = false;
  if (!obj->GetInstanceTemplate().IsUndefined(isolate)) {
    Handle<ObjectTemplateInfo> GetInstanceTemplate = Handle<ObjectTemplateInfo>(
        ObjectTemplateInfo::cast(obj->GetInstanceTemplate()), isolate);
    embedder_field_count = GetInstanceTemplate->embedder_field_count();
    immutable_proto = GetInstanceTemplate->immutable_proto();
  }

  // JSFunction requires information about the prototype slot.
  DCHECK(!InstanceTypeChecker::IsJSFunction(type));
  int instance_size = JSObject::GetHeaderSize(type) +
                      kEmbedderDataSlotSize * embedder_field_count;

  Handle<Map> map = isolate->factory()->NewMap(type, instance_size,
                                               TERMINAL_FAST_ELEMENTS_KIND);

  // Mark as undetectable if needed.
  if (obj->undetectable()) {
    // We only allow callable undetectable receivers here, since this whole
    // undetectable business is only to support document.all, which is both
    // undetectable and callable. If we ever see the need to have an object
    // that is undetectable but not callable, we need to update the types.h
    // to allow encoding this.
    CHECK(!obj->GetInstanceCallHandler().IsUndefined(isolate));
    map->set_is_undetectable(true);
  }

  // Mark as needs_access_check if needed.
  if (obj->needs_access_check()) {
    map->set_is_access_check_needed(true);
    map->set_may_have_interesting_symbols(true);
  }

  // Set interceptor information in the map.
  if (!obj->GetNamedPropertyHandler().IsUndefined(isolate)) {
    map->set_has_named_interceptor(true);
    map->set_may_have_interesting_symbols(true);
  }
  if (!obj->GetIndexedPropertyHandler().IsUndefined(isolate)) {
    map->set_has_indexed_interceptor(true);
  }

  // Mark instance as callable in the map.
  if (!obj->GetInstanceCallHandler().IsUndefined(isolate)) {
    map->set_is_callable(true);
    map->set_is_constructor(!obj->undetectable());
  }

  if (immutable_proto) map->set_is_immutable_proto(true);

  JSFunction::SetInitialMap(isolate, result, map,
                            Handle<JSObject>::cast(prototype));
  return result;
}

}  // namespace internal
}  // namespace v8

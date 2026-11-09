// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_dom_module.h"

#include <limits>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/raw_ref.h"
#include "components/python_runtime/python_runtime.h"

namespace native_python {
namespace {

constexpr Py_ssize_t kStringLimit = 16 * 1024;

struct ModuleState {
  // CPython zero-initializes module state; no C++ constructors run here.
  RAW_PTR_EXCLUSION DomHost* host;
  RAW_PTR_EXCLUSION PyTypeObject* handle_type;
  RAW_PTR_EXCLUSION PyTypeObject* event_type;
  RAW_PTR_EXCLUSION PyTypeObject* listener_type;
  RAW_PTR_EXCLUSION PyObject* callbacks;
  uint64_t context_id;
  uint64_t next_callback_id;
};

struct ElementHandle {
  PyObject_HEAD DomHandle value;
};

struct ListenerHandle {
  PyObject_HEAD DomHandle element;
  CallbackHandle callback;
};

class ScopedHostRevocation {
 public:
  explicit ScopedHostRevocation(ModuleState& state)
      : state_(state), host_(std::exchange(state.host, nullptr)) {}
  ~ScopedHostRevocation() { state_->host = host_; }
  DomHost* host() const { return host_; }

 private:
  const raw_ref<ModuleState> state_;
  RAW_PTR_EXCLUSION DomHost* host_;
};

struct CallbackEventObject {
  PyObject_HEAD
      // CPython allocates zeroed storage and its member descriptors require raw
      // Python reference fields, not C++ pointer wrappers.
      RAW_PTR_EXCLUSION PyObject* type;
  RAW_PTR_EXCLUSION PyObject* current_target;
  char is_trusted;
};

PyObject* MakeHandle(ModuleState* state, DomHandle value) {
  auto* handle = reinterpret_cast<ElementHandle*>(
      state->handle_type->tp_alloc(state->handle_type, 0));
  if (!handle) {
    return nullptr;
  }
  handle->value = value;
  return reinterpret_cast<PyObject*>(handle);
}

bool StringArgument(PyObject* object, std::string_view* result) {
  if (!PyUnicode_Check(object)) {
    PyErr_SetString(PyExc_TypeError, "DOM arguments must be str");
    return false;
  }
  if (PyUnicode_GetLength(object) > kStringLimit) {
    PyErr_SetString(PyExc_ValueError,
                    "DOM strings must not exceed 16 KiB UTF-8");
    return false;
  }
  Py_ssize_t size;
  const char* text = PyUnicode_AsUTF8AndSize(object, &size);
  if (!text) {
    return false;
  }
  if (size > kStringLimit) {
    PyErr_SetString(PyExc_ValueError,
                    "DOM strings must not exceed 16 KiB UTF-8");
    return false;
  }
  *result = std::string_view(text, size);
  return true;
}

ModuleState* ActiveState(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state || !state->host || !state->host->IsActive()) {
    PyErr_SetString(PyExc_ReferenceError,
                    "The document DOM context is inactive");
    return nullptr;
  }
  return state;
}

bool CheckResult(const DomResult& result) {
  PyObject* exception = nullptr;
  switch (result.error) {
    case DomError::kNone:
      return true;
    case DomError::kReference:
      exception = PyExc_ReferenceError;
      break;
    case DomError::kNotAllowed:
      exception = PyExc_PermissionError;
      break;
    case DomError::kLimit:
      exception = PyExc_ValueError;
      break;
    case DomError::kState:
    case DomError::kStopped:
      exception = PyExc_RuntimeError;
      break;
  }
  PyErr_SetString(exception, result.message.c_str());
  return false;
}

PyObject* GetElementById(PyObject* module, PyObject* args) {
  PyObject* id;
  if (!PyArg_ParseTuple(args, "O:get_element_by_id", &id)) {
    return nullptr;
  }
  std::string_view text;
  if (!StringArgument(id, &text)) {
    return nullptr;
  }
  auto* state = ActiveState(module);
  if (!state) {
    return nullptr;
  }
  const auto result = state->host->GetElementById(text);
  if (!CheckResult(result)) {
    return nullptr;
  }
  if (!result.handle.id) {
    Py_RETURN_NONE;
  }
  return MakeHandle(state, result.handle);
}

PyObject* SetText(PyObject* module, PyObject* args) {
  PyObject *node, *text;
  if (!PyArg_ParseTuple(args, "OO:set_text", &node, &text)) {
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state || !state->handle_type || !Py_IS_TYPE(node, state->handle_type)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's ElementHandle");
    return nullptr;
  }
  std::string_view value;
  if (!StringArgument(text, &value) || !ActiveState(module)) {
    return nullptr;
  }
  if (!CheckResult(state->host->SetText(
          reinterpret_cast<ElementHandle*>(node)->value, value))) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyObject* AddEventListener(PyObject* module, PyObject* args, PyObject* kwargs) {
  PyObject *element, *type, *callback;
  PyObject* once = Py_False;
  static const char* names[] = {"element", "event_type", "callback", "once",
                                nullptr};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOO|$O:add_event_listener",
                                   names, &element, &type, &callback, &once)) {
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state || !state->handle_type ||
      !Py_IS_TYPE(element, state->handle_type)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's ElementHandle");
    return nullptr;
  }
  std::string_view event_type;
  if (!StringArgument(type, &event_type)) {
    return nullptr;
  }
  if (event_type != "click") {
    PyErr_SetString(PyExc_ValueError, "Only click listeners are supported");
    return nullptr;
  }
  if (!PyBool_Check(once)) {
    PyErr_SetString(PyExc_TypeError, "once must be bool");
    return nullptr;
  }
  if (!ActiveState(module)) {
    return nullptr;
  }
  ScopedHostRevocation revoked(*state);
  // Allocate the result before installing either side of the subscription.
  auto* listener = reinterpret_cast<ListenerHandle*>(
      state->listener_type->tp_alloc(state->listener_type, 0));
  if (!listener) {
    return nullptr;
  }
  listener->element = reinterpret_cast<ElementHandle*>(element)->value;
  listener->callback = RegisterDomCallback(module, callback, once == Py_True);
  if (!listener->callback.id) {
    Py_DECREF(listener);
    return nullptr;
  }
  const auto result = revoked.host()->AddEventListener(
      listener->element, event_type, listener->callback, once == Py_True);
  if (!CheckResult(result)) {
    // The wrapper's post-call check can fail after the host installed a
    // listener. Revoke even after a fatal stop, then release Python ownership
    // without allowing finalizers to inherit the caller's DOM capability.
    PyObject* failure = PyErr_GetRaisedException();
    revoked.host()->RevokeEventListener(listener->element, listener->callback);
    RemoveDomCallback(module, listener->callback);
    Py_DECREF(listener);
    PyErr_Clear();
    PyErr_SetRaisedException(failure);
    return nullptr;
  }
  return reinterpret_cast<PyObject*>(listener);
}

PyObject* RemoveEventListener(PyObject* module, PyObject* args) {
  PyObject* object;
  if (!PyArg_ParseTuple(args, "O:remove_event_listener", &object)) {
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state || !state->listener_type ||
      !Py_IS_TYPE(object, state->listener_type)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's ListenerHandle");
    return nullptr;
  }
  if (!ActiveState(module)) {
    return nullptr;
  }
  const auto* listener = reinterpret_cast<ListenerHandle*>(object);
  if (listener->callback.context != state->context_id) {
    PyErr_SetString(PyExc_ReferenceError,
                    "The listener belongs to another context");
    return nullptr;
  }
  ScopedHostRevocation revoked(*state);
  if (!CheckResult(revoked.host()->RemoveEventListener(listener->element,
                                                       listener->callback)) ||
      !RemoveDomCallback(module, listener->callback)) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

void DeleteHandle(PyObject* self) {
  PyTypeObject* type = Py_TYPE(self);
  PyObject_GC_UnTrack(self);
  type->tp_free(self);
  Py_DECREF(type);
}

int TraverseHandle(PyObject* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  return 0;
}

int TraverseEvent(PyObject* self, visitproc visit, void* arg) {
  auto* event = reinterpret_cast<CallbackEventObject*>(self);
  Py_VISIT(Py_TYPE(self));
  Py_VISIT(event->type);
  Py_VISIT(event->current_target);
  return 0;
}

int ClearEvent(PyObject* self) {
  auto* event = reinterpret_cast<CallbackEventObject*>(self);
  Py_CLEAR(event->type);
  Py_CLEAR(event->current_target);
  return 0;
}

void DeleteEvent(PyObject* self) {
  PyTypeObject* type = Py_TYPE(self);
  PyObject_GC_UnTrack(self);
  ClearEvent(self);
  type->tp_free(self);
  Py_DECREF(type);
}

PyMemberDef kEventMembers[] = {
    {"type", Py_T_OBJECT_EX, offsetof(CallbackEventObject, type), Py_READONLY,
     nullptr},
    {"current_target", Py_T_OBJECT_EX,
     offsetof(CallbackEventObject, current_target), Py_READONLY, nullptr},
    {"is_trusted", Py_T_BOOL, offsetof(CallbackEventObject, is_trusted),
     Py_READONLY, nullptr},
    {nullptr}};

PyType_Slot kEventSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteEvent)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseEvent)},
    {Py_tp_clear, reinterpret_cast<void*>(ClearEvent)},
    {Py_tp_members, kEventMembers},
    {0, nullptr}};
PyType_Spec kEventSpec = {"dom.CallbackEvent", sizeof(CallbackEventObject), 0,
                          Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                              Py_TPFLAGS_IMMUTABLETYPE |
                              Py_TPFLAGS_DISALLOW_INSTANTIATION,
                          kEventSlots};

PyType_Slot kHandleSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteHandle)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseHandle)},
    {0, nullptr}};
PyType_Spec kHandleSpec = {"dom.ElementHandle", sizeof(ElementHandle), 0,
                           Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                               Py_TPFLAGS_IMMUTABLETYPE |
                               Py_TPFLAGS_DISALLOW_INSTANTIATION,
                           kHandleSlots};

PyType_Spec kListenerSpec = {"dom.ListenerHandle", sizeof(ListenerHandle), 0,
                             Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                                 Py_TPFLAGS_IMMUTABLETYPE |
                                 Py_TPFLAGS_DISALLOW_INSTANTIATION,
                             kHandleSlots};

int Initialize(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  state->handle_type = reinterpret_cast<PyTypeObject*>(
      PyType_FromModuleAndSpec(module, &kHandleSpec, nullptr));
  state->event_type = reinterpret_cast<PyTypeObject*>(
      PyType_FromModuleAndSpec(module, &kEventSpec, nullptr));
  state->listener_type = reinterpret_cast<PyTypeObject*>(
      PyType_FromModuleAndSpec(module, &kListenerSpec, nullptr));
  state->callbacks = PyDict_New();
  if (!state->handle_type || !state->event_type || !state->listener_type ||
      !state->callbacks ||
      PyModule_AddObjectRef(module, "ElementHandle",
                            reinterpret_cast<PyObject*>(state->handle_type)) <
          0 ||
      PyModule_AddObjectRef(module, "ListenerHandle",
                            reinterpret_cast<PyObject*>(state->listener_type)) <
          0) {
    return -1;
  }
  return PyModule_AddObjectRef(module, "CallbackEvent",
                               reinterpret_cast<PyObject*>(state->event_type));
}

int Traverse(PyObject* module, visitproc visit, void* arg) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  Py_VISIT(state->handle_type);
  Py_VISIT(state->event_type);
  Py_VISIT(state->listener_type);
  Py_VISIT(state->callbacks);
  return 0;
}

int Clear(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  state->host = nullptr;
  // Detach the table before decrefs: finalizers must see a cleared registry.
  Py_CLEAR(state->callbacks);
  Py_CLEAR(state->event_type);
  Py_CLEAR(state->listener_type);
  Py_CLEAR(state->handle_type);
  return 0;
}

PyMethodDef kMethods[] = {
    {"get_element_by_id", GetElementById, METH_VARARGS, nullptr},
    {"set_text", SetText, METH_VARARGS, nullptr},
    {"add_event_listener", reinterpret_cast<PyCFunction>(AddEventListener),
     METH_VARARGS | METH_KEYWORDS, nullptr},
    {"remove_event_listener", RemoveEventListener, METH_VARARGS, nullptr},
    {nullptr}};
PyModuleDef_Slot kSlots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(Initialize)},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {0, nullptr}};
PyModuleDef kModule = {PyModuleDef_HEAD_INIT,
                       "dom",
                       nullptr,
                       sizeof(ModuleState),
                       kMethods,
                       kSlots,
                       Traverse,
                       Clear,
                       nullptr};

}  // namespace

PyMODINIT_FUNC InitDomModule() {
  return PyModuleDef_Init(&kModule);
}

void SetDomHost(PyObject* module, DomHost* host) {
  ExchangeDomHost(module, host);
}

DomHost* ExchangeDomHost(PyObject* module, DomHost* host) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  DomHost* previous = state->host;
  state->host = host;
  return previous;
}

void InitializeDomCallbacks(PyObject* module, uint64_t context_id) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  CHECK(context_id);
  CHECK_EQ(state->context_id, 0u);
  state->context_id = context_id;
}

CallbackHandle RegisterDomCallback(PyObject* module,
                                   PyObject* callable,
                                   bool once) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state->callbacks || !state->context_id) {
    PyErr_SetString(PyExc_ReferenceError, "The callback registry is inactive");
    return {};
  }
  if (!PyCallable_Check(callable)) {
    PyErr_SetString(PyExc_TypeError, "The callback must be callable");
    return {};
  }
  PyObject* function =
      PyMethod_Check(callable) ? PyMethod_GET_FUNCTION(callable) : callable;
  if (PyFunction_Check(function) &&
      (reinterpret_cast<PyCodeObject*>(PyFunction_GetCode(function))->co_flags &
       (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR |
        CO_ITERABLE_COROUTINE))) {
    PyErr_SetString(PyExc_TypeError,
                    "Only synchronous callbacks are supported");
    return {};
  }
  if (PyDict_Size(state->callbacks) >= 128) {
    PyErr_SetString(PyExc_ValueError,
                    "At most 128 callbacks are accepted per context");
    return {};
  }
  CHECK_NE(state->next_callback_id, std::numeric_limits<uint64_t>::max());
  const uint64_t id = ++state->next_callback_id;
  PyObject* key = PyLong_FromUnsignedLongLong(id);
  if (!key) {
    return {};
  }
  PyObject* entry = PyTuple_Pack(2, callable, once ? Py_True : Py_False);
  const int status = entry ? PyDict_SetItem(state->callbacks, key, entry) : -1;
  Py_XDECREF(entry);
  Py_DECREF(key);
  return status == 0 ? CallbackHandle{state->context_id, id} : CallbackHandle{};
}

PyObject* GetDomCallback(PyObject* module, CallbackHandle handle) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state->callbacks || !handle.id || handle.context != state->context_id) {
    PyErr_SetString(PyExc_ReferenceError,
                    "The callback belongs to another context or is inactive");
    return nullptr;
  }
  PyObject* key = PyLong_FromUnsignedLongLong(handle.id);
  if (!key) {
    return nullptr;
  }
  PyObject* entry = nullptr;
  const int found = PyDict_GetItemRef(state->callbacks, key, &entry);
  PyObject* callback = nullptr;
  if (found == 1) {
    // Keep the callable alive across table removal, exceptions and
    // self-removal.
    callback = Py_NewRef(PyTuple_GetItem(entry, 0));
    if (PyTuple_GetItem(entry, 1) == Py_True &&
        PyDict_DelItem(state->callbacks, key) < 0) {
      Py_CLEAR(callback);
    }
  }
  Py_XDECREF(entry);
  Py_DECREF(key);
  if (found == 0) {
    PyErr_SetString(PyExc_ReferenceError, "The callback was removed");
  }
  return callback;
}

bool RemoveDomCallback(PyObject* module, CallbackHandle handle) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state->callbacks || !handle.id || handle.context != state->context_id) {
    PyErr_SetString(PyExc_ReferenceError,
                    "The callback belongs to another context or is inactive");
    return false;
  }
  PyObject* key = PyLong_FromUnsignedLongLong(handle.id);
  if (!key) {
    return false;
  }
  PyObject* callback = nullptr;
  const int found = PyDict_Pop(state->callbacks, key, &callback);
  Py_DECREF(key);
  // The table is consistent before releasing arbitrary callable instances.
  Py_XDECREF(callback);
  return found >= 0;
}

PyObject* CreateDomCallbackEvent(PyObject* module, const CallbackEvent& event) {
  if (event.type != "click" || !event.current_target.document ||
      !event.current_target.generation || !event.current_target.id) {
    PyErr_SetString(PyExc_ValueError,
                    "Expected a click event with a valid element handle");
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  auto* object = reinterpret_cast<CallbackEventObject*>(
      state->event_type->tp_alloc(state->event_type, 0));
  if (!object) {
    return nullptr;
  }
  object->type = PyUnicode_FromString("click");
  object->current_target = MakeHandle(state, event.current_target);
  object->is_trusted = event.is_trusted;
  if (!object->type || !object->current_target) {
    Py_DECREF(object);
    return nullptr;
  }
  return reinterpret_cast<PyObject*>(object);
}

void ClearDomCallbacks(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  Py_CLEAR(state->callbacks);
}

}  // namespace native_python

// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_dom_module.h"

#include <cmath>
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
  RAW_PTR_EXCLUSION PyTypeObject* document_type;
  RAW_PTR_EXCLUSION PyTypeObject* window_type;
  RAW_PTR_EXCLUSION PyTypeObject* page_type;
  RAW_PTR_EXCLUSION PyTypeObject* event_type;
  RAW_PTR_EXCLUSION PyTypeObject* listener_type;
  RAW_PTR_EXCLUSION PyTypeObject* frame_type;
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

struct FrameHandle {
  PyObject_HEAD CallbackHandle callback;
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
  PyObject_HEAD RAW_PTR_EXCLUSION PyObject* owner;
  RAW_PTR_EXCLUSION PyObject* current_target;
  RAW_PTR_EXCLUSION PyObject* type;
  char is_trusted;
  unsigned long long control_token;
  char cancelable;
  char bubbles;
  char default_prevented;
  char passive;
  char target_is_editable;
  char target_in_dialog;
  RAW_PTR_EXCLUSION PyObject* key;
  RAW_PTR_EXCLUSION PyObject* code;
  char repeat;
  char is_composing;
  char alt_key;
  char ctrl_key;
  char meta_key;
  char shift_key;
  int pointer_id;
  RAW_PTR_EXCLUSION PyObject* pointer_type;
  short button;
  unsigned short buttons;
  double client_x;
  double client_y;
  char is_primary;
  RAW_PTR_EXCLUSION PyObject* value;
  char checked;
  char value_truncated;
  unsigned long long input_epoch;
};

PyObject* MakeHandle(ModuleState* state, DomHandle value) {
  auto* type = value.kind == TargetKind::kDocument ? state->document_type
               : value.kind == TargetKind::kWindow ? state->window_type
                                                   : state->handle_type;
  auto* handle = reinterpret_cast<ElementHandle*>(type->tp_alloc(type, 0));
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

bool SupportedEvent(TargetKind kind, std::string_view type) {
  if (kind == TargetKind::kDocument) {
    return type == "keydown" || type == "keyup" || type == "visibilitychange";
  }
  if (kind == TargetKind::kWindow) {
    return type == "blur" || type == "focus" || type == "resize";
  }
  return kind == TargetKind::kElement &&
         (type == "click" || type == "input" || type == "change" ||
          type == "pointerdown" || type == "pointerup" ||
          type == "pointermove" || type == "pointercancel" ||
          type == "gotpointercapture" || type == "lostpointercapture");
}

bool TargetArgument(ModuleState* state, PyObject* object, DomHandle* value) {
  if (!state || !(Py_IS_TYPE(object, state->handle_type) ||
                  Py_IS_TYPE(object, state->document_type) ||
                  Py_IS_TYPE(object, state->window_type))) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's event target handle");
    return false;
  }
  *value = reinterpret_cast<ElementHandle*>(object)->value;
  return true;
}

PyObject* GetTarget(PyObject* module, TargetKind kind) {
  auto* state = ActiveState(module);
  if (!state) {
    return nullptr;
  }
  const auto result = state->host->GetEventTarget(kind);
  if (!CheckResult(result)) {
    return nullptr;
  }
  if (result.handle.kind != kind || !result.handle.document ||
      !result.handle.generation || !result.handle.id) {
    PyErr_SetString(PyExc_ReferenceError, "Invalid document or window handle");
    return nullptr;
  }
  return MakeHandle(state, result.handle);
}
PyObject* GetDocument(PyObject* module, PyObject*) {
  return GetTarget(module, TargetKind::kDocument);
}
PyObject* GetWindow(PyObject* module, PyObject*) {
  return GetTarget(module, TargetKind::kWindow);
}

PyObject* PointerCapture(PyObject* module, PyObject* args, bool capture) {
  PyObject *target, *pointer;
  if (!PyArg_ParseTuple(args, "OO:pointer_capture", &target, &pointer)) {
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state || !Py_IS_TYPE(target, state->handle_type)) {
    PyErr_SetString(PyExc_TypeError,
                    "Expected this interpreter's ElementHandle");
    return nullptr;
  }
  if (!PyLong_CheckExact(pointer)) {
    PyErr_SetString(PyExc_TypeError, "pointer_id must be an int");
    return nullptr;
  }
  const long long id = PyLong_AsLongLong(pointer);
  if (PyErr_Occurred()) {
    return nullptr;
  }
  if (id < std::numeric_limits<int32_t>::min() ||
      id > std::numeric_limits<int32_t>::max()) {
    PyErr_SetString(PyExc_ValueError, "pointer_id is outside int32 range");
    return nullptr;
  }
  if (!ActiveState(module) ||
      !CheckResult(state->host->SetPointerCapture(
          reinterpret_cast<ElementHandle*>(target)->value,
          static_cast<int32_t>(id), capture))) {
    return nullptr;
  }
  Py_RETURN_NONE;
}
PyObject* SetPointerCapture(PyObject* m, PyObject* args) {
  return PointerCapture(m, args, true);
}
PyObject* ReleasePointerCapture(PyObject* m, PyObject* args) {
  return PointerCapture(m, args, false);
}

PyObject* ControlEvent(PyObject* self, EventAction action) {
  auto* event = reinterpret_cast<CallbackEventObject*>(self);
  auto* state = event->owner ? ActiveState(event->owner) : nullptr;
  if (!state) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_ReferenceError, "The event is inactive");
    }
    return nullptr;
  }
  if (!CheckResult(state->host->ControlEvent(event->control_token, action))) {
    return nullptr;
  }
  Py_RETURN_NONE;
}
PyObject* PreventDefault(PyObject* self, PyObject*) {
  return ControlEvent(self, EventAction::kPreventDefault);
}
PyObject* StopPropagation(PyObject* self, PyObject*) {
  return ControlEvent(self, EventAction::kStopPropagation);
}
PyObject* StopImmediatePropagation(PyObject* self, PyObject*) {
  return ControlEvent(self, EventAction::kStopImmediatePropagation);
}
PyMethodDef kEventMethods[] = {
    {"prevent_default", PreventDefault, METH_NOARGS, nullptr},
    {"stop_propagation", StopPropagation, METH_NOARGS, nullptr},
    {"stop_immediate_propagation", StopImmediatePropagation, METH_NOARGS,
     nullptr},
    {nullptr}};

PyObject* AddEventListener(PyObject* module, PyObject* args, PyObject* kwargs) {
  PyObject *element, *type, *callback;
  PyObject* once = Py_False;
  PyObject* passive = Py_False;
  PyObject* capture = Py_False;
  static const char* names[] = {"element", "event_type", "callback", "once",
                                "passive", "capture",    nullptr};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOO|$OOO:add_event_listener",
                                   names, &element, &type, &callback, &once,
                                   &passive, &capture)) {
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  DomHandle target;
  if (!TargetArgument(state, element, &target)) {
    return nullptr;
  }
  std::string_view event_type;
  if (!StringArgument(type, &event_type)) {
    return nullptr;
  }
  if (!SupportedEvent(target.kind, event_type)) {
    PyErr_SetString(PyExc_ValueError,
                    "Event type is not supported for this target");
    return nullptr;
  }
  if (!PyBool_Check(once) || !PyBool_Check(passive) || !PyBool_Check(capture)) {
    PyErr_SetString(PyExc_TypeError, "once, passive and capture must be bool");
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
  listener->element = target;
  listener->callback = RegisterDomCallback(module, callback, once == Py_True);
  if (!listener->callback.id) {
    Py_DECREF(listener);
    return nullptr;
  }
  const auto result = revoked.host()->AddEventListener(
      listener->element, event_type, listener->callback, once == Py_True,
      passive == Py_True, capture == Py_True);
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
  Py_VISIT(event->owner);
  Py_VISIT(event->current_target);
  Py_VISIT(event->type);
  Py_VISIT(event->key);
  Py_VISIT(event->code);
  Py_VISIT(event->pointer_type);
  Py_VISIT(event->value);
  return 0;
}

int ClearEvent(PyObject* self) {
  auto* event = reinterpret_cast<CallbackEventObject*>(self);
  Py_CLEAR(event->owner);
  Py_CLEAR(event->current_target);
  Py_CLEAR(event->type);
  Py_CLEAR(event->key);
  Py_CLEAR(event->code);
  Py_CLEAR(event->pointer_type);
  Py_CLEAR(event->value);
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
    {"current_target", Py_T_OBJECT_EX,
     offsetof(CallbackEventObject, current_target), Py_READONLY, nullptr},
    {"type", Py_T_OBJECT_EX, offsetof(CallbackEventObject, type), Py_READONLY,
     nullptr},
    {"is_trusted", Py_T_BOOL, offsetof(CallbackEventObject, is_trusted),
     Py_READONLY, nullptr},
    {"cancelable", Py_T_BOOL, offsetof(CallbackEventObject, cancelable),
     Py_READONLY, nullptr},
    {"bubbles", Py_T_BOOL, offsetof(CallbackEventObject, bubbles), Py_READONLY,
     nullptr},
    {"default_prevented", Py_T_BOOL,
     offsetof(CallbackEventObject, default_prevented), Py_READONLY, nullptr},
    {"passive", Py_T_BOOL, offsetof(CallbackEventObject, passive), Py_READONLY,
     nullptr},
    {"target_is_editable", Py_T_BOOL,
     offsetof(CallbackEventObject, target_is_editable), Py_READONLY, nullptr},
    {"target_in_dialog", Py_T_BOOL,
     offsetof(CallbackEventObject, target_in_dialog), Py_READONLY, nullptr},
    {"key", Py_T_OBJECT_EX, offsetof(CallbackEventObject, key), Py_READONLY,
     nullptr},
    {"code", Py_T_OBJECT_EX, offsetof(CallbackEventObject, code), Py_READONLY,
     nullptr},
    {"repeat", Py_T_BOOL, offsetof(CallbackEventObject, repeat), Py_READONLY,
     nullptr},
    {"is_composing", Py_T_BOOL, offsetof(CallbackEventObject, is_composing),
     Py_READONLY, nullptr},
    {"alt_key", Py_T_BOOL, offsetof(CallbackEventObject, alt_key), Py_READONLY,
     nullptr},
    {"ctrl_key", Py_T_BOOL, offsetof(CallbackEventObject, ctrl_key),
     Py_READONLY, nullptr},
    {"meta_key", Py_T_BOOL, offsetof(CallbackEventObject, meta_key),
     Py_READONLY, nullptr},
    {"shift_key", Py_T_BOOL, offsetof(CallbackEventObject, shift_key),
     Py_READONLY, nullptr},
    {"pointer_id", Py_T_INT, offsetof(CallbackEventObject, pointer_id),
     Py_READONLY, nullptr},
    {"pointer_type", Py_T_OBJECT_EX,
     offsetof(CallbackEventObject, pointer_type), Py_READONLY, nullptr},
    {"button", Py_T_SHORT, offsetof(CallbackEventObject, button), Py_READONLY,
     nullptr},
    {"buttons", Py_T_USHORT, offsetof(CallbackEventObject, buttons),
     Py_READONLY, nullptr},
    {"client_x", Py_T_DOUBLE, offsetof(CallbackEventObject, client_x),
     Py_READONLY, nullptr},
    {"client_y", Py_T_DOUBLE, offsetof(CallbackEventObject, client_y),
     Py_READONLY, nullptr},
    {"is_primary", Py_T_BOOL, offsetof(CallbackEventObject, is_primary),
     Py_READONLY, nullptr},
    {"value", Py_T_OBJECT_EX, offsetof(CallbackEventObject, value), Py_READONLY,
     nullptr},
    {"checked", Py_T_BOOL, offsetof(CallbackEventObject, checked), Py_READONLY,
     nullptr},
    {"value_truncated", Py_T_BOOL,
     offsetof(CallbackEventObject, value_truncated), Py_READONLY, nullptr},
    {"input_epoch", Py_T_ULONGLONG, offsetof(CallbackEventObject, input_epoch),
     Py_READONLY, nullptr},
    {nullptr}};

PyType_Slot kEventSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteEvent)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseEvent)},
    {Py_tp_clear, reinterpret_cast<void*>(ClearEvent)},
    {Py_tp_members, kEventMembers},
    {Py_tp_methods, kEventMethods},
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

PyType_Spec kFrameSpec = {"animation.FrameHandle", sizeof(FrameHandle), 0,
                          Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                              Py_TPFLAGS_IMMUTABLETYPE |
                              Py_TPFLAGS_DISALLOW_INSTANTIATION,
                          kHandleSlots};

PyType_Spec kDocumentSpec = {"dom.DocumentHandle", sizeof(ElementHandle), 0,
                             Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                                 Py_TPFLAGS_IMMUTABLETYPE |
                                 Py_TPFLAGS_DISALLOW_INSTANTIATION,
                             kHandleSlots};
PyType_Spec kWindowSpec = {"dom.WindowHandle", sizeof(ElementHandle), 0,
                           Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                               Py_TPFLAGS_IMMUTABLETYPE |
                               Py_TPFLAGS_DISALLOW_INSTANTIATION,
                           kHandleSlots};
struct PageStateObject {
  PyObject_HEAD char hidden;
  char has_focus;
  int width;
  int height;
  char reduced_motion;
  unsigned long long input_epoch;
};
PyMemberDef kPageMembers[] = {
    {"hidden", Py_T_BOOL, offsetof(PageStateObject, hidden), Py_READONLY,
     nullptr},
    {"has_focus", Py_T_BOOL, offsetof(PageStateObject, has_focus), Py_READONLY,
     nullptr},
    {"width", Py_T_INT, offsetof(PageStateObject, width), Py_READONLY, nullptr},
    {"height", Py_T_INT, offsetof(PageStateObject, height), Py_READONLY,
     nullptr},
    {"reduced_motion", Py_T_BOOL, offsetof(PageStateObject, reduced_motion),
     Py_READONLY, nullptr},
    {"input_epoch", Py_T_ULONGLONG, offsetof(PageStateObject, input_epoch),
     Py_READONLY, nullptr},
    {nullptr}};
PyType_Slot kPageSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteHandle)},
    {Py_tp_traverse, reinterpret_cast<void*>(TraverseHandle)},
    {Py_tp_members, kPageMembers},
    {0, nullptr}};
PyType_Spec kPageSpec = {"dom.PageState", sizeof(PageStateObject), 0,
                         Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                             Py_TPFLAGS_IMMUTABLETYPE |
                             Py_TPFLAGS_DISALLOW_INSTANTIATION,
                         kPageSlots};

PyObject* GetPageState(PyObject* module, PyObject*) {
  auto* state = ActiveState(module);
  if (!state) {
    return nullptr;
  }
  const auto result = state->host->GetPageState();
  if (!CheckResult(result.status)) {
    return nullptr;
  }
  auto* value = reinterpret_cast<PageStateObject*>(
      state->page_type->tp_alloc(state->page_type, 0));
  if (!value) {
    return nullptr;
  }
  value->hidden = result.state.hidden;
  value->has_focus = result.state.has_focus;
  value->width = result.state.width;
  value->height = result.state.height;
  value->reduced_motion = result.state.reduced_motion;
  value->input_epoch = result.state.input_epoch;
  return reinterpret_cast<PyObject*>(value);
}

int Initialize(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  auto add = [&](PyTypeObject*& type, PyType_Spec& spec, const char* name) {
    type = reinterpret_cast<PyTypeObject*>(
        PyType_FromModuleAndSpec(module, &spec, nullptr));
    return type &&
           (!name || PyModule_AddObjectRef(
                         module, name, reinterpret_cast<PyObject*>(type)) == 0);
  };
  state->callbacks = PyDict_New();
  return state->callbacks &&
                 add(state->handle_type, kHandleSpec, "ElementHandle") &&
                 add(state->document_type, kDocumentSpec, "DocumentHandle") &&
                 add(state->window_type, kWindowSpec, "WindowHandle") &&
                 add(state->event_type, kEventSpec, "CallbackEvent") &&
                 add(state->listener_type, kListenerSpec, "ListenerHandle") &&
                 add(state->frame_type, kFrameSpec, nullptr) &&
                 add(state->page_type, kPageSpec, "PageState")
             ? 0
             : -1;
}

int Traverse(PyObject* module, visitproc visit, void* arg) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  Py_VISIT(state->document_type);
  Py_VISIT(state->window_type);
  Py_VISIT(state->page_type);
  Py_VISIT(state->handle_type);
  Py_VISIT(state->event_type);
  Py_VISIT(state->listener_type);
  Py_VISIT(state->frame_type);
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
  Py_CLEAR(state->frame_type);
  Py_CLEAR(state->document_type);
  Py_CLEAR(state->window_type);
  Py_CLEAR(state->page_type);
  Py_CLEAR(state->handle_type);
  return 0;
}

PyMethodDef kMethods[] = {
    {"get_document", GetDocument, METH_NOARGS, nullptr},
    {"get_window", GetWindow, METH_NOARGS, nullptr},
    {"get_page_state", GetPageState, METH_NOARGS, nullptr},
    {"set_pointer_capture", SetPointerCapture, METH_VARARGS, nullptr},
    {"release_pointer_capture", ReleasePointerCapture, METH_VARARGS, nullptr},
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

struct AnimationState {
  // Share the interpreter's DOM capability and callback table. This module
  // cannot keep a second host alive after ScopedDomHost revokes the first.
  RAW_PTR_EXCLUSION PyObject* dom;
};

PyObject* RequestFrame(PyObject* module, PyObject* callback) {
  auto* animation = static_cast<AnimationState*>(PyModule_GetState(module));
  if (!animation->dom) {
    PyErr_SetString(PyExc_ReferenceError, "The animation module is inactive");
    return nullptr;
  }
  auto* state = ActiveState(animation->dom);
  if (!state) {
    return nullptr;
  }
  ScopedHostRevocation revoked(*state);
  auto* frame = reinterpret_cast<FrameHandle*>(
      state->frame_type->tp_alloc(state->frame_type, 0));
  if (!frame) {
    return nullptr;
  }
  frame->callback = RegisterDomCallback(animation->dom, callback, true,
                                        ExecutionKind::kAnimationFrame);
  if (!frame->callback.id) {
    Py_DECREF(frame);
    return nullptr;
  }
  if (!CheckResult(revoked.host()->RequestFrame(frame->callback))) {
    PyObject* failure = PyErr_GetRaisedException();
    revoked.host()->RevokeFrame(frame->callback);
    RemoveDomCallback(animation->dom, frame->callback);
    Py_DECREF(frame);
    PyErr_Clear();
    PyErr_SetRaisedException(failure);
    return nullptr;
  }
  return reinterpret_cast<PyObject*>(frame);
}

PyObject* CancelFrame(PyObject* module, PyObject* object) {
  auto* animation = static_cast<AnimationState*>(PyModule_GetState(module));
  if (!animation->dom) {
    PyErr_SetString(PyExc_ReferenceError, "The animation module is inactive");
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(animation->dom));
  if (!state->frame_type || !Py_IS_TYPE(object, state->frame_type)) {
    PyErr_SetString(PyExc_TypeError, "Expected this interpreter's FrameHandle");
    return nullptr;
  }
  if (!ActiveState(animation->dom)) {
    return nullptr;
  }
  auto callback = reinterpret_cast<FrameHandle*>(object)->callback;
  if (callback.context != state->context_id) {
    PyErr_SetString(PyExc_ReferenceError,
                    "The frame belongs to another context");
    return nullptr;
  }
  ScopedHostRevocation revoked(*state);
  if (!CheckResult(revoked.host()->CancelFrame(callback)) ||
      !RemoveDomCallback(animation->dom, callback)) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

int InitializeAnimation(PyObject* module) {
  auto* state = static_cast<AnimationState*>(PyModule_GetState(module));
  state->dom = PyImport_ImportModule("dom");
  if (!state->dom) {
    return -1;
  }
  if (!PyModule_Check(state->dom) || PyModule_GetDef(state->dom) != &kModule) {
    PyErr_SetString(PyExc_TypeError,
                    "animation requires the native dom module");
    return -1;
  }
  auto* dom = static_cast<ModuleState*>(PyModule_GetState(state->dom));
  return PyModule_AddObjectRef(module, "FrameHandle",
                               reinterpret_cast<PyObject*>(dom->frame_type));
}

int TraverseAnimation(PyObject* module, visitproc visit, void* arg) {
  auto* state = static_cast<AnimationState*>(PyModule_GetState(module));
  Py_VISIT(state->dom);
  return 0;
}

int ClearAnimation(PyObject* module) {
  auto* state = static_cast<AnimationState*>(PyModule_GetState(module));
  Py_CLEAR(state->dom);
  return 0;
}

PyMethodDef kAnimationMethods[] = {
    {"request_frame", RequestFrame, METH_O, nullptr},
    {"cancel_frame", CancelFrame, METH_O, nullptr},
    {nullptr}};
PyModuleDef_Slot kAnimationSlots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(InitializeAnimation)},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {0, nullptr}};
PyModuleDef kAnimationModule = {
    PyModuleDef_HEAD_INIT,  "animation",       nullptr,
    sizeof(AnimationState), kAnimationMethods, kAnimationSlots,
    TraverseAnimation,      ClearAnimation,    nullptr};

}  // namespace

PyMODINIT_FUNC InitDomModule() {
  return PyModuleDef_Init(&kModule);
}

PyMODINIT_FUNC InitAnimationModule() {
  return PyModuleDef_Init(&kAnimationModule);
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
                                   bool once,
                                   ExecutionKind kind) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  if (!state->callbacks || !state->context_id) {
    PyErr_SetString(PyExc_ReferenceError, "The callback registry is inactive");
    return {};
  }
  if (kind == ExecutionKind::kInitialization) {
    PyErr_SetString(PyExc_ValueError, "Invalid callback kind");
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
  PyObject* purpose = PyLong_FromLong(static_cast<long>(kind));
  PyObject* entry =
      purpose ? PyTuple_Pack(3, callable, once ? Py_True : Py_False, purpose)
              : nullptr;
  Py_XDECREF(purpose);
  const int status = entry ? PyDict_SetItem(state->callbacks, key, entry) : -1;
  Py_XDECREF(entry);
  Py_DECREF(key);
  return status == 0 ? CallbackHandle{state->context_id, id} : CallbackHandle{};
}

PyObject* GetDomCallback(PyObject* module,
                         CallbackHandle handle,
                         ExecutionKind kind) {
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
  if (found == 1 &&
      PyLong_AsLong(PyTuple_GetItem(entry, 2)) != static_cast<long>(kind)) {
    PyErr_SetString(PyExc_TypeError,
                    "Callback kind does not match the entry point");
  } else if (found == 1) {
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
  if (!SupportedEvent(event.current_target.kind, event.type) ||
      !event.current_target.document || !event.current_target.generation ||
      !event.current_target.id || !std::isfinite(event.client_x) ||
      !std::isfinite(event.client_y)) {
    PyErr_SetString(PyExc_ValueError,
                    "Expected a supported event with a valid target and finite "
                    "coordinates");
    return nullptr;
  }
  if (event.type.size() > 16384) {
    PyErr_SetString(PyExc_ValueError, "Event strings must not exceed 16 KiB");
    return nullptr;
  }
  if (event.key.size() > 16384) {
    PyErr_SetString(PyExc_ValueError, "Event strings must not exceed 16 KiB");
    return nullptr;
  }
  if (event.code.size() > 16384) {
    PyErr_SetString(PyExc_ValueError, "Event strings must not exceed 16 KiB");
    return nullptr;
  }
  if (event.pointer_type.size() > 16384) {
    PyErr_SetString(PyExc_ValueError, "Event strings must not exceed 16 KiB");
    return nullptr;
  }
  if (event.value.size() > 16384) {
    PyErr_SetString(PyExc_ValueError, "Event strings must not exceed 16 KiB");
    return nullptr;
  }
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  auto* object = reinterpret_cast<CallbackEventObject*>(
      state->event_type->tp_alloc(state->event_type, 0));
  if (!object) {
    return nullptr;
  }
  object->owner = Py_NewRef(module);
  object->current_target = MakeHandle(state, event.current_target);
  object->type =
      PyUnicode_DecodeUTF8(event.type.data(), event.type.size(), "strict");
  object->is_trusted = event.is_trusted;
  object->control_token = event.control_token;
  object->cancelable = event.cancelable;
  object->bubbles = event.bubbles;
  object->default_prevented = event.default_prevented;
  object->passive = event.passive;
  object->target_is_editable = event.target_is_editable;
  object->target_in_dialog = event.target_in_dialog;
  object->key =
      PyUnicode_DecodeUTF8(event.key.data(), event.key.size(), "strict");
  object->code =
      PyUnicode_DecodeUTF8(event.code.data(), event.code.size(), "strict");
  object->repeat = event.repeat;
  object->is_composing = event.is_composing;
  object->alt_key = event.alt_key;
  object->ctrl_key = event.ctrl_key;
  object->meta_key = event.meta_key;
  object->shift_key = event.shift_key;
  object->pointer_id = event.pointer_id;
  object->pointer_type = PyUnicode_DecodeUTF8(
      event.pointer_type.data(), event.pointer_type.size(), "strict");
  object->button = event.button;
  object->buttons = event.buttons;
  object->client_x = event.client_x;
  object->client_y = event.client_y;
  object->is_primary = event.is_primary;
  object->value =
      PyUnicode_DecodeUTF8(event.value.data(), event.value.size(), "strict");
  object->checked = event.checked;
  object->value_truncated = event.value_truncated;
  object->input_epoch = event.input_epoch;
  if (!object->current_target || !object->type || !object->key ||
      !object->code || !object->pointer_type || !object->value) {
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

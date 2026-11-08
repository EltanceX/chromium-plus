// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_dom_module.h"

#include "base/memory/raw_ptr_exclusion.h"
#include "components/python_runtime/python_runtime.h"

namespace native_python {
namespace {

constexpr Py_ssize_t kStringLimit = 16 * 1024;

struct ModuleState {
  // CPython zero-initializes module state; no C++ constructors run here.
  RAW_PTR_EXCLUSION DomHost* host;
  RAW_PTR_EXCLUSION PyTypeObject* handle_type;
};

struct ElementHandle {
  PyObject_HEAD DomHandle value;
};

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
  auto* handle = reinterpret_cast<ElementHandle*>(
      state->handle_type->tp_alloc(state->handle_type, 0));
  if (!handle) {
    return nullptr;
  }
  handle->value = result.handle;
  return reinterpret_cast<PyObject*>(handle);
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

void DeleteHandle(PyObject* self) {
  PyTypeObject* type = Py_TYPE(self);
  type->tp_free(self);
  Py_DECREF(type);
}

PyType_Slot kHandleSlots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(DeleteHandle)},
    {0, nullptr}};
PyType_Spec kHandleSpec = {"dom.ElementHandle", sizeof(ElementHandle), 0,
                           Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE |
                               Py_TPFLAGS_DISALLOW_INSTANTIATION,
                           kHandleSlots};

int Initialize(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  state->handle_type = reinterpret_cast<PyTypeObject*>(
      PyType_FromModuleAndSpec(module, &kHandleSpec, nullptr));
  return state->handle_type
             ? PyModule_AddObjectRef(
                   module, "ElementHandle",
                   reinterpret_cast<PyObject*>(state->handle_type))
             : -1;
}

int Traverse(PyObject* module, visitproc visit, void* arg) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  Py_VISIT(state->handle_type);
  return 0;
}

int Clear(PyObject* module) {
  auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
  state->host = nullptr;
  Py_CLEAR(state->handle_type);
  return 0;
}

PyMethodDef kMethods[] = {
    {"get_element_by_id", GetElementById, METH_VARARGS, nullptr},
    {"set_text", SetText, METH_VARARGS, nullptr},
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
  static_cast<ModuleState*>(PyModule_GetState(module))->host = host;
}

}  // namespace native_python

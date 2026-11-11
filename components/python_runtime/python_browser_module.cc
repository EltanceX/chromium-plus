// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>

#include "base/memory/raw_ptr_exclusion.h"
#include "components/python_runtime/python_dom_module.h"

namespace native_python {
namespace {
struct State {
  RAW_PTR_EXCLUSION PyObject* dom;
};
State* Data(PyObject* m) {
  return static_cast<State*>(PyModule_GetState(m));
}
bool Text(PyObject* object, size_t limit, std::string* out) {
  if (!PyUnicode_Check(object)) {
    PyErr_SetString(PyExc_TypeError, "Expected text");
    return false;
  }
  if (PyUnicode_GetLength(object) > static_cast<Py_ssize_t>(limit)) {
    PyErr_SetString(PyExc_ValueError, "Browser text exceeds its limit");
    return false;
  }
  Py_ssize_t size;
  const char* text = PyUnicode_AsUTF8AndSize(object, &size);
  if (!text) {
    return false;
  }
  if (size > static_cast<Py_ssize_t>(limit) ||
      std::string_view(text, size).find('\0') != std::string_view::npos) {
    PyErr_SetString(PyExc_ValueError,
                    "Browser text is too long or contains NUL");
    return false;
  }
  out->assign(text, size);
  return true;
}
PyObject* Run(PyObject* module, const BrowserRequest& request) {
  auto* state = Data(module);
  DomHost* host = state->dom ? ActiveDomHost(state->dom) : nullptr;
  if (!host) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_ReferenceError, "Browser module is inactive");
    }
    return nullptr;
  }
  const auto r = host->Browser(request);
  if (!CheckDomResult(r.status)) {
    return nullptr;
  }
  switch (request.op) {
    case BrowserOp::kAudioResume:
    case BrowserOp::kAudioSuspend:
    case BrowserOp::kRequestFullscreen:
    case BrowserOp::kExitFullscreen:
      return PyLong_FromUnsignedLongLong(r.id);
    case BrowserOp::kStorageGet:
      if (!r.flag) {
        Py_RETURN_NONE;
      }
      [[fallthrough]];
    case BrowserOp::kAudioState:
      return PyUnicode_DecodeUTF8(r.value.data(), r.value.size(), "strict");
    case BrowserOp::kIsFullscreen:
      return PyBool_FromLong(r.flag);
    case BrowserOp::kPoll:
      if (!r.id) {
        Py_RETURN_NONE;
      }
      return Py_BuildValue("KOs", static_cast<unsigned long long>(r.id),
                           r.flag ? Py_True : Py_False, r.value.c_str());
    default:
      Py_RETURN_NONE;
  }
}
template <BrowserOp op>
PyObject* NoArgs(PyObject* m, PyObject*) {
  return Run(m, {op});
}
PyObject* StorageGet(PyObject* m, PyObject* key) {
  BrowserRequest r{BrowserOp::kStorageGet};
  return Text(key, 256, &r.key) ? Run(m, r) : nullptr;
}
PyObject* StorageSet(PyObject* m, PyObject* args) {
  PyObject *key, *value;
  if (!PyArg_ParseTuple(args, "OO:storage_set", &key, &value)) {
    return nullptr;
  }
  BrowserRequest r{BrowserOp::kStorageSet};
  return Text(key, 256, &r.key) && Text(value, 16384, &r.value) ? Run(m, r)
                                                                : nullptr;
}
PyObject* Fullscreen(PyObject* m, PyObject* element) {
  BrowserRequest r{BrowserOp::kRequestFullscreen};
  return Data(m)->dom && ReadDomElementHandle(Data(m)->dom, element, &r.element)
             ? Run(m, r)
             : nullptr;
}
PyObject* Export(PyObject* m, PyObject* name) {
  BrowserRequest r{BrowserOp::kExportDocument};
  return Text(name, 96, &r.key) ? Run(m, r) : nullptr;
}
PyObject* Tones(PyObject* m, PyObject* sequence) {
  if (!PyList_CheckExact(sequence) && !PyTuple_CheckExact(sequence)) {
    PyErr_SetString(PyExc_TypeError, "Expected a list or tuple of tones");
    return nullptr;
  }
  const Py_ssize_t count = PySequence_Size(sequence);
  if (count < 1 || count > 16) {
    PyErr_SetString(PyExc_ValueError, "Expected 1 to 16 tones");
    return nullptr;
  }
  BrowserRequest r{BrowserOp::kPlayTones};
  for (Py_ssize_t i = 0; i < count; ++i) {
    PyObject* item = PyList_Check(sequence) ? PyList_GetItem(sequence, i)
                                            : PyTuple_GetItem(sequence, i);
    if (!item) {
      return nullptr;
    }
    if (!PyTuple_CheckExact(item) || PyTuple_Size(item) != 6) {
      PyErr_SetString(PyExc_TypeError, "Expected a six-item tone tuple");
      return nullptr;
    }
    // Conversion hooks could mutate the outer list and release a borrowed
    // tuple while its remaining fields are being parsed. Accept plain data.
    for (Py_ssize_t field = 1; field < 6; ++field) {
      PyObject* number = PyTuple_GetItem(item, field);
      if (!PyFloat_CheckExact(number) && !PyLong_CheckExact(number)) {
        PyErr_SetString(PyExc_TypeError, "Tone numbers must be int or float");
        return nullptr;
      }
    }
    PyObject* wave;
    AudioTone t;
    if (!PyArg_ParseTuple(item, "Oddddd:play_tones", &wave, &t.frequency,
                          &t.end_frequency, &t.duration, &t.delay, &t.volume) ||
        !Text(wave, 16, &t.wave)) {
      return nullptr;
    }
    if ((t.wave != "sine" && t.wave != "square" && t.wave != "triangle" &&
         t.wave != "sawtooth") ||
        !std::isfinite(t.frequency) || !std::isfinite(t.end_frequency) ||
        !std::isfinite(t.duration) || !std::isfinite(t.delay) ||
        !std::isfinite(t.volume) || t.frequency < 20 || t.frequency > 20000 ||
        t.end_frequency < 20 || t.end_frequency > 20000 || t.duration < .01 ||
        t.duration > 2 || t.delay < 0 || t.delay > 4 || t.volume < 0 ||
        t.volume > .25) {
      PyErr_SetString(PyExc_ValueError, "Invalid bounded synthesis tone");
      return nullptr;
    }
    r.tones.push_back(std::move(t));
  }
  return Run(m, r);
}
int Traverse(PyObject* m, visitproc visit, void* arg) {
  Py_VISIT(Data(m)->dom);
  return 0;
}
int Clear(PyObject* m) {
  Py_CLEAR(Data(m)->dom);
  return 0;
}
int Initialize(PyObject* m) {
  Data(m)->dom = PyImport_ImportModule("dom");
  return Data(m)->dom ? 0 : -1;
}
PyMethodDef kMethods[] = {
    {"audio_resume", NoArgs<BrowserOp::kAudioResume>, METH_NOARGS, nullptr},
    {"audio_suspend", NoArgs<BrowserOp::kAudioSuspend>, METH_NOARGS, nullptr},
    {"audio_stop", NoArgs<BrowserOp::kAudioStop>, METH_NOARGS, nullptr},
    {"audio_state", NoArgs<BrowserOp::kAudioState>, METH_NOARGS, nullptr},
    {"play_tones", Tones, METH_O, nullptr},
    {"storage_get", StorageGet, METH_O, nullptr},
    {"storage_set", StorageSet, METH_VARARGS, nullptr},
    {"request_fullscreen", Fullscreen, METH_O, nullptr},
    {"exit_fullscreen", NoArgs<BrowserOp::kExitFullscreen>, METH_NOARGS,
     nullptr},
    {"is_fullscreen", NoArgs<BrowserOp::kIsFullscreen>, METH_NOARGS, nullptr},
    {"export_document", Export, METH_O, nullptr},
    {"poll", NoArgs<BrowserOp::kPoll>, METH_NOARGS, nullptr},
    {nullptr}};
PyModuleDef_Slot kSlots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(Initialize)},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {0, nullptr}};
PyModuleDef kModule = {PyModuleDef_HEAD_INIT,
                       "browser",
                       nullptr,
                       sizeof(State),
                       kMethods,
                       kSlots,
                       Traverse,
                       Clear,
                       nullptr};
}  // namespace
PyMODINIT_FUNC InitBrowserModule() {
  return PyModuleDef_Init(&kModule);
}
}  // namespace native_python

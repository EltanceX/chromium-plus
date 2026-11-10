// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_DOM_MODULE_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_DOM_MODULE_H_

#include <Python.h>

#include "components/python_runtime/python_runtime.h"

namespace native_python {
class DomHost;
PyMODINIT_FUNC InitDomModule();
PyMODINIT_FUNC InitCanvasModule();
DomHost* ActiveDomHost(PyObject* dom_module);
bool ReadDomElementHandle(PyObject* dom_module, PyObject*, DomHandle*);
bool CheckDomResult(const DomResult&);
PyMODINIT_FUNC InitAnimationModule();
void SetDomHost(PyObject* module, DomHost* host);
DomHost* ExchangeDomHost(PyObject* module, DomHost* host);
void InitializeDomCallbacks(PyObject* module, uint64_t context_id);
// Internal CPython operations: the owning interpreter and GIL must be active.
// All returned PyObject references are owned by the caller.
CallbackHandle RegisterDomCallback(
    PyObject* module,
    PyObject* callable,
    bool once = false,
    ExecutionKind kind = ExecutionKind::kCallback);
// Acquires a temporary callable reference and consumes a once entry before
// returning. The caller must not grant DOM capability until this completes.
PyObject* GetDomCallback(PyObject* module,
                         CallbackHandle,
                         ExecutionKind kind = ExecutionKind::kCallback);
bool RemoveDomCallback(PyObject* module, CallbackHandle);
PyObject* CreateDomCallbackEvent(PyObject* module, const CallbackEvent&);
void ClearDomCallbacks(PyObject* module);
}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_DOM_MODULE_H_

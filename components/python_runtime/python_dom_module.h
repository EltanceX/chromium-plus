// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_DOM_MODULE_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_DOM_MODULE_H_

#include <Python.h>

namespace native_python {
class DomHost;
PyMODINIT_FUNC InitDomModule();
void SetDomHost(PyObject* module, DomHost* host);
}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_DOM_MODULE_H_

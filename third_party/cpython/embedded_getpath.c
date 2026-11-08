// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Python.h"
#include "pycore_initconfig.h"
#include "pycore_pathconfig.h"

// Do not probe the executable directory, environment, registry or disk. All
// supported modules are built in or frozen, and the embedder supplies paths.
PyStatus _PyConfig_InitPathConfig(PyConfig* config, int compute_path_config) {
  if (!config->isolated || config->use_environment ||
      !config->module_search_paths_set || config->module_search_paths.length ||
      !config->prefix || !config->exec_prefix || !config->executable) {
    return PyStatus_Error("embedded CPython requires explicit isolated paths");
  }
  return PyStatus_Ok();
}

PyObject* _Py_Get_Getpath_CodeObject(void) {
  PyErr_SetString(PyExc_RuntimeError, "disk path discovery is disabled");
  return NULL;
}

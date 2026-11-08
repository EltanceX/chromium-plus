// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_runtime.h"

#include <Python.h>

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"

namespace native_python {

BASE_FEATURE(kNativePythonRuntimeSelfTest, base::FEATURE_DISABLED_BY_DEFAULT);

namespace {

constexpr size_t kOutputLimit = 16 * 1024;
constexpr size_t kSourceLimit = 64 * 1024;

struct PyDeleter {
  void operator()(PyObject* value) const { Py_XDECREF(value); }
};
using PyRef = std::unique_ptr<PyObject, PyDeleter>;

void AppendBounded(std::string& target,
                   std::string_view text,
                   size_t available) {
  target.append(text.substr(0, available));
  while (!base::IsStringUTF8(target)) {
    target.pop_back();
  }
}

std::string Text(PyObject* object) {
  if (!object) {
    PyErr_Clear();
    return "<unavailable>";
  }
  PyRef rendered(PyObject_Str(object));
  const char* text = rendered ? PyUnicode_AsUTF8(rendered.get()) : nullptr;
  if (!text) {
    PyErr_Clear();
    return "<unprintable>";
  }
  std::string result;
  AppendBounded(result, text, kOutputLimit);
  return result;
}

std::string TakeError() {
  PyRef exception(PyErr_GetRaisedException());
  if (!exception) {
    return "CPython operation failed without an exception";
  }
  std::string message = "Traceback (most recent call last):\n";
  PyRef traceback(PyException_GetTraceback(exception.get()));
  for (int frames = 0; traceback && traceback.get() != Py_None && frames < 32;
       ++frames) {
    PyRef frame(PyObject_GetAttrString(traceback.get(), "tb_frame"));
    PyRef line(PyObject_GetAttrString(traceback.get(), "tb_lineno"));
    PyRef code(frame ? PyObject_GetAttrString(frame.get(), "f_code") : nullptr);
    PyRef filename(code ? PyObject_GetAttrString(code.get(), "co_filename")
                        : nullptr);
    PyRef name(code ? PyObject_GetAttrString(code.get(), "co_name") : nullptr);
    message += "  File " + Text(filename.get()) + ", line " + Text(line.get()) +
               ", in " + Text(name.get()) + "\n";
    traceback.reset(PyObject_GetAttrString(traceback.get(), "tb_next"));
    if (message.size() > kOutputLimit) {
      break;
    }
  }
  message += std::string(Py_TYPE(exception.get())->tp_name) + ": " +
             Text(exception.get());
  if (PyErr_GivenExceptionMatches(exception.get(), PyExc_SyntaxError)) {
    PyRef filename(PyObject_GetAttrString(exception.get(), "filename"));
    PyRef line(PyObject_GetAttrString(exception.get(), "lineno"));
    message += " (" + Text(filename.get()) + ":" + Text(line.get()) + ")";
  }
  PyErr_Clear();
  std::string bounded;
  AppendBounded(bounded, message, kOutputLimit);
  return bounded;
}

struct Capture {
  ExecutionResult result;
  bool active = true;
};

struct HostState {
  // CPython allocates this C module state as zeroed bytes, without C++ ctors.
  // Capture stays alive until Py_EndInterpreter returns.
  RAW_PTR_EXCLUSION Capture* capture = nullptr;
};

PyObject* Write(PyObject* module, PyObject* args) {
  int stderr_stream;
  PyObject* text;
  if (!PyArg_ParseTuple(args, "iU", &stderr_stream, &text)) {
    return nullptr;
  }
  auto* state = static_cast<HostState*>(PyModule_GetState(module));
  if (!state || !state->capture || !state->capture->active) {
    // Finalizers must not call a host callback during interpreter teardown.
    return PyLong_FromSsize_t(PyUnicode_GetLength(text));
  }
  Py_ssize_t length = 0;
  const char* bytes = PyUnicode_AsUTF8AndSize(text, &length);
  if (!bytes) {
    return nullptr;
  }
  auto& result = state->capture->result;
  const size_t remaining =
      kOutputLimit - result.stdout_text.size() - result.stderr_text.size();
  result.output_truncated |= static_cast<size_t>(length) > remaining;
  AppendBounded(stderr_stream ? result.stderr_text : result.stdout_text,
                std::string_view(bytes, length), remaining);
  return PyLong_FromSsize_t(PyUnicode_GetLength(text));
}

PyMethodDef kMethods[] = {{"write", Write, METH_VARARGS, nullptr}, {nullptr}};
PyModuleDef_Slot kSlots[] = {
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {0, nullptr}};
PyModuleDef kHostModule = {PyModuleDef_HEAD_INIT,
                           "_chromium_python",
                           nullptr,
                           sizeof(HostState),
                           kMethods,
                           kSlots,
                           nullptr,
                           nullptr,
                           nullptr};
PyMODINIT_FUNC InitHostModule() {
  return PyModuleDef_Init(&kHostModule);
}

constexpr char kCaptureSetup[] = R"PY(
import sys as _sys
import _chromium_python as _host
class _Writer:
    encoding = 'utf-8'
    errors = 'strict'
    def __init__(self, stream):
        self.stream = stream
    def write(self, text):
        return _host.write(self.stream, text)
    def flush(self):
        pass
    def isatty(self):
        return False
_sys.stdout = _Writer(0)
_sys.stderr = _Writer(1)
_sys.__stdout__ = _sys.stdout
_sys.__stderr__ = _sys.stderr
)PY";

class Runtime {
 public:
  static Runtime& Get() {
    static base::NoDestructor<Runtime> runtime;
    return *runtime;
  }
  bool Initialize(std::string* error) {
    CHECK_EQ(thread_, base::PlatformThread::CurrentId());
    if (main) {
      return true;
    }
    if (attempted_) {
      *error = failure_;
      return false;
    }
    attempted_ = true;
    const auto start = base::TimeTicks::Now();
    CHECK(!Py_IsInitialized());
    PyPreConfig preconfig;
    PyPreConfig_InitIsolatedConfig(&preconfig);
    preconfig.utf8_mode = 1;
    PyStatus status = Py_PreInitialize(&preconfig);
    if (!PyStatus_Exception(status) &&
        PyImport_AppendInittab("_chromium_python", InitHostModule) != 0) {
      status = PyStatus_Error("failed to register native output module");
    }
    if (!PyStatus_Exception(status)) {
      PyConfig config;
      PyConfig_InitIsolatedConfig(&config);
      config.site_import = 0;
      config.user_site_directory = 0;
      config.install_signal_handlers = 0;
      config.configure_c_stdio = 0;
      config.write_bytecode = 0;
      config.use_frozen_modules = 1;
      config.module_search_paths_set = 1;
      config.pathconfig_warnings = 0;
      for (wchar_t** field :
           {&config.program_name, &config.executable, &config.base_executable,
            &config.home, &config.prefix, &config.base_prefix,
            &config.exec_prefix, &config.base_exec_prefix,
            &config.stdlib_dir}) {
        status = PyConfig_SetString(&config, field, L"<chromium-embedded>");
        if (PyStatus_Exception(status)) {
          break;
        }
      }
      if (!PyStatus_Exception(status)) {
        status = PyConfig_SetString(&config, &config.stdio_encoding, L"utf-8");
      }
      if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
      }
      PyConfig_Clear(&config);
    }
    if (PyStatus_Exception(status)) {
      failure_ =
          status.err_msg ? status.err_msg : "CPython initialization failed";
      *error = failure_;
      return false;
    }
    main = PyEval_SaveThread();
    initialization_ms = (base::TimeTicks::Now() - start).InMillisecondsF();
    return true;
  }
  void CheckThread() { CHECK_EQ(thread_, base::PlatformThread::CurrentId()); }

  // CPython owns this state; the process runtime is deliberately never
  // finalized.
  RAW_PTR_EXCLUSION PyThreadState* main = nullptr;
  double initialization_ms = 0;

 private:
  friend class base::NoDestructor<Runtime>;
  Runtime() : thread_(base::PlatformThread::CurrentId()) {}
  const base::PlatformThreadId thread_;
  bool attempted_ = false;
  std::string failure_;
};

}  // namespace

struct PythonContext::Impl {
  // CPython owns thread; globals owns a Python reference, released under its
  // GIL.
  RAW_PTR_EXCLUSION PyThreadState* thread = nullptr;
  RAW_PTR_EXCLUSION PyObject* globals = nullptr;
  Capture capture;
};

PythonContext::PythonContext(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

std::unique_ptr<PythonContext> PythonContext::Create(std::string* error) {
  CHECK(error);
  auto& runtime = Runtime::Get();
  if (!runtime.Initialize(error)) {
    return nullptr;
  }
  PyEval_RestoreThread(runtime.main);
  auto impl = std::make_unique<Impl>();
  const PyInterpreterConfig config = {
      0, 0, 0, 0, 0, 1, PyInterpreterConfig_SHARED_GIL};
  const PyStatus status = Py_NewInterpreterFromConfig(&impl->thread, &config);
  if (PyStatus_Exception(status)) {
    *error = status.err_msg ? status.err_msg : "subinterpreter creation failed";
    PyEval_SaveThread();  // CPython restores the caller on failure.
    return nullptr;
  }
  bool setup_ok = false;
  {
    PyRef host(PyImport_ImportModule("_chromium_python"));
    if (host) {
      static_cast<HostState*>(PyModule_GetState(host.get()))->capture =
          &impl->capture;
      PyRef setup_globals(PyDict_New());
      PyRef result(setup_globals
                       ? PyRun_String(kCaptureSetup, Py_file_input,
                                      setup_globals.get(), setup_globals.get())
                       : nullptr);
      impl->globals = PyDict_New();
      if (impl->globals) {
        PyRef name(PyUnicode_FromString("__main__"));
        setup_ok =
            name &&
            PyDict_SetItemString(impl->globals, "__name__", name.get()) == 0 &&
            PyDict_SetItemString(impl->globals, "__builtins__",
                                 PyEval_GetBuiltins()) == 0 &&
            result;
      }
    }
    if (!setup_ok) {
      *error = TakeError();
    }
  }
  if (!setup_ok) {
    impl->capture.active = false;
    Py_XDECREF(impl->globals);
    Py_EndInterpreter(impl->thread);
    return nullptr;
  }
  PyEval_SaveThread();
  return std::unique_ptr<PythonContext>(new PythonContext(std::move(impl)));
}

PythonContext::~PythonContext() {
  Runtime::Get().CheckThread();
  PyEval_RestoreThread(impl_->thread);
  impl_->capture.active = false;
  Py_DECREF(impl_->globals);
  Py_EndInterpreter(impl_->thread);
}

ExecutionResult PythonContext::Execute(std::string_view source,
                                       std::string_view filename) {
  Runtime::Get().CheckThread();
  if (source.size() > kSourceLimit || source.contains('\0') ||
      filename.contains('\0') || filename.size() > 2048 ||
      !base::IsStringUTF8(source) || !base::IsStringUTF8(filename)) {
    ExecutionResult result;
    result.error = "Source/filename must be bounded UTF-8 without NUL";
    return result;
  }
  PyEval_RestoreThread(impl_->thread);
  impl_->capture.result = {};
  {
    const std::string input(source), name(filename);
    PyRef code(Py_CompileString(input.c_str(), name.c_str(), Py_file_input));
    PyRef result(
        code ? PyEval_EvalCode(code.get(), impl_->globals, impl_->globals)
             : nullptr);
    impl_->capture.result.success = result != nullptr;
    if (!result) {
      impl_->capture.result.error = TakeError();
    }
  }
  PyEval_SaveThread();
  return std::move(impl_->capture.result);
}

bool IsInitialized() {
  return Runtime::Get().main != nullptr;
}
std::string Version() {
  return PY_VERSION;
}

base::DictValue RunSelfTest() {
  base::DictValue report;
  report.Set("version", Version());
  const auto start = base::TimeTicks::Now();
  std::string error;
  auto first = PythonContext::Create(&error);
  auto second = first ? PythonContext::Create(&error) : nullptr;
  if (!first || !second) {
    report.Set("passed", false);
    report.Set("error", error);
    return report;
  }
  auto calculation =
      first->Execute("answer = 6 * 7\nprint('Python 世界', answer)\n");
  auto persistence = first->Execute("assert answer == 42\n");
  auto isolation = second->Execute(
      "assert 'answer' not in globals()\nimport sys\nassert not "
      "sys.path\nassert sys.flags.isolated and not "
      "sys.flags.ignore_environment == 0\nassert 'site' not in "
      "sys.modules\nsys._phase_one_marker = 42\n");
  auto modules = first->Execute(
      "import sys\nassert not hasattr(sys, '_phase_one_marker')\n");
  auto exception =
      first->Execute("def fail():\n    raise ValueError('phase-one')\nfail()\n",
                     "<phase-one-error>");
  auto output = second->Execute("print('界' * 10000)\n");
  first.reset();
  second.reset();
  auto replacement = PythonContext::Create(&error);
  auto cleanup = replacement ? replacement->Execute("assert 6 * 7 == 42")
                             : ExecutionResult();
  replacement.reset();
  const bool passed =
      calculation.success && calculation.stdout_text == "Python 世界 42\n" &&
      persistence.success && isolation.success && modules.success &&
      !exception.success && exception.error.contains("ValueError") &&
      exception.error.contains("<phase-one-error>") && output.success &&
      output.output_truncated && output.stdout_text.size() <= kOutputLimit &&
      base::IsStringUTF8(output.stdout_text) && cleanup.success;
  report.Set("passed", passed);
  report.Set("stdout", calculation.stdout_text);
  report.Set("traceback", exception.error);
  report.Set("output_truncated", output.output_truncated);
  report.Set("isolated_contexts", isolation.success && modules.success);
  report.Set("context_cleanup", cleanup.success);
  if (!passed) {
    report.Set("error", error + calculation.error + persistence.error +
                            isolation.error + modules.error + output.error +
                            cleanup.error);
  }
  report.Set("initialization_ms", Runtime::Get().initialization_ms);
  report.Set("elapsed_ms", (base::TimeTicks::Now() - start).InMillisecondsF());
  return report;
}

}  // namespace native_python

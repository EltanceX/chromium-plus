// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_runtime.h"

#include <Python.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/function_ref.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/platform_thread.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "components/python_runtime/python_dom_module.h"
#include "components/python_runtime/python_execution_budget.h"

namespace native_python {

BASE_FEATURE(kNativePythonRuntimeSelfTest, base::FEATURE_DISABLED_BY_DEFAULT);

DomResult DomHost::RequestFrame(CallbackHandle) {
  return {{}, DomError::kNotAllowed, "Animation frames are not supported"};
}

DomResult DomHost::CancelFrame(CallbackHandle) {
  return {{}, DomError::kNotAllowed, "Animation frames are not supported"};
}

namespace {

constexpr size_t kOutputLimit = 16 * 1024;
constexpr size_t kSourceLimit = 64 * 1024;

struct PyDeleter {
  void operator()(PyObject* value) const { Py_XDECREF(value); }
};
using PyRef = std::unique_ptr<PyObject, PyDeleter>;

// Both pointers are borrowed within an entered interpreter. Revocation must
// precede diagnostic work and destruction of arbitrary Python values.
class ScopedDomHost {
 public:
  ScopedDomHost(PyObject* module, DomHost* host)
      : module_(module), previous_(ExchangeDomHost(module, host)) {}
  ~ScopedDomHost() { SetDomHost(module_, previous_); }
  ScopedDomHost(const ScopedDomHost&) = delete;
  ScopedDomHost& operator=(const ScopedDomHost&) = delete;

 private:
  RAW_PTR_EXCLUSION PyObject* module_;
  RAW_PTR_EXCLUSION DomHost* previous_;
};

void AppendBounded(std::string& target,
                   std::string_view text,
                   size_t available) {
  target.append(text.substr(0, available));
  while (!base::IsStringUTF8(target)) {
    target.pop_back();
  }
}

std::string Text(PyObject* object, ExecutionBudget* budget) {
  if (budget && !budget->Check()) {
    return budget->Message();
  }
  if (!object) {
    PyErr_Clear();
    return "<unavailable>";
  }
  PyRef rendered(PyObject_Str(object));
  Py_ssize_t length = 0;
  const char* text =
      rendered ? PyUnicode_AsUTF8AndSize(rendered.get(), &length) : nullptr;
  if (!text) {
    PyErr_Clear();
    return "<unprintable>";
  }
  if (budget && static_cast<size_t>(length) > budget->OutputRemaining()) {
    budget->Stop(StopReason::kOutput);
  }
  std::string result;
  AppendBounded(result, text, kOutputLimit);
  return result;
}

std::string TakeError(ExecutionBudget* budget = nullptr) {
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
    message += "  File " + Text(filename.get(), budget) + ", line " +
               Text(line.get(), budget) + ", in " + Text(name.get(), budget) +
               "\n";
    traceback.reset(PyObject_GetAttrString(traceback.get(), "tb_next"));
    if (message.size() > kOutputLimit) {
      break;
    }
  }
  message += std::string(Py_TYPE(exception.get())->tp_name) + ": " +
             Text(exception.get(), budget);
  if (PyErr_GivenExceptionMatches(exception.get(), PyExc_SyntaxError)) {
    PyRef filename(PyObject_GetAttrString(exception.get(), "filename"));
    PyRef line(PyObject_GetAttrString(exception.get(), "lineno"));
    message += " (" + Text(filename.get(), budget) + ":" +
               Text(line.get(), budget) + ")";
  }
  PyErr_Clear();
  std::string bounded;
  if (budget && message.size() > budget->OutputRemaining()) {
    budget->Stop(StopReason::kOutput);
  }
  AppendBounded(bounded, message, kOutputLimit);
  return bounded;
}

void RecordError(ExecutionResult& result, ExecutionBudget& budget) {
  if (budget.reason() == StopReason::kNone) {
    const std::string error = TakeError(&budget);
    AppendBounded(result.error, error, budget.OutputRemaining());
    result.output_truncated |= !budget.ConsumeOutput(error.size());
  } else {
    PyErr_Clear();
  }
}

void RecordStop(ExecutionResult& result, const ExecutionBudget& budget) {
  result.stop_reason = budget.reason();
  if (result.stop_reason != StopReason::kNone) {
    result.success = false;
    result.error = budget.Message();
    result.output_truncated |= result.stop_reason == StopReason::kOutput;
  }
}

struct Capture {
  explicit Capture(const base::TickClock* clock) : budget(clock) {}
  ExecutionResult result;
  ExecutionBudget budget;
  bool active = false;
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
  auto& budget = state->capture->budget;
  if (budget.RaiseIfStopped()) {
    return nullptr;
  }
  Py_ssize_t length = 0;
  const char* bytes = PyUnicode_AsUTF8AndSize(text, &length);
  if (!bytes) {
    return nullptr;
  }
  auto& result = state->capture->result;
  const size_t remaining = budget.OutputRemaining();
  result.output_truncated |= static_cast<size_t>(length) > remaining;
  AppendBounded(stderr_stream ? result.stderr_text : result.stdout_text,
                std::string_view(bytes, length), remaining);
  if (!budget.ConsumeOutput(length)) {
    budget.RaiseIfStopped();
    return nullptr;
  }
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
    if (!PyStatus_Exception(status) &&
        (PyImport_AppendInittab("dom", InitDomModule) != 0 ||
         PyImport_AppendInittab("animation", InitAnimationModule) != 0)) {
      status = PyStatus_Error("failed to register native DOM module");
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
  bool executing = false;
  uint64_t active_context_id = 0;
  uint64_t next_context_id = 0;
  RuntimeStatistics statistics;

 private:
  friend class base::NoDestructor<Runtime>;
  Runtime() : thread_(base::PlatformThread::CurrentId()) {}
  const base::PlatformThreadId thread_;
  bool attempted_ = false;
  std::string failure_;
};

}  // namespace

struct PythonContext::Impl {
  Impl(const base::TickClock* clock, uint64_t id)
      : context_id(id), capture(clock) {}
  ExecutionResult Run(DomHost* host,
                      base::FunctionRef<PyObject*(DomHost*)> operation,
                      ExecutionKind kind = ExecutionKind::kInitialization);
  ExecutionResult Call(CallbackHandle,
                       ExecutionKind,
                       DomHost*,
                       base::FunctionRef<PyObject*()> make_argument);
  // CPython owns thread; globals owns a Python reference, released under its
  // GIL.
  RAW_PTR_EXCLUSION PyThreadState* thread = nullptr;
  RAW_PTR_EXCLUSION PyObject* globals = nullptr;
  RAW_PTR_EXCLUSION PyObject* dom_module = nullptr;
  RAW_PTR_EXCLUSION PyObject* trace_token = nullptr;
  const uint64_t context_id;
  Capture capture;
};

ExecutionResult PythonContext::Impl::Run(
    DomHost* host,
    base::FunctionRef<PyObject*(DomHost*)> operation,
    ExecutionKind kind) {
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  if (runtime.executing) {
    ExecutionResult result;
    result.error = "Nested Python entry is not supported";
    return result;
  }
  auto& budget = capture.budget;
  if (budget.reason() != StopReason::kNone) {
    ExecutionResult result;
    RecordStop(result, budget);
    return result;
  }
  if (!budget.Begin(host, kind)) {
    ExecutionResult result;
    result.callback_skipped = true;
    return result;
  }
  PyEval_RestoreThread(thread);
  base::AutoReset<bool> executing(&runtime.executing, true);
  base::AutoReset<uint64_t> active_context(&runtime.active_context_id,
                                           context_id);
  capture.result = {};
  capture.active = true;
  BudgetedDomHost bridge(budget);
  PyEval_SetTrace(BudgetTrace, trace_token);
  {
    ScopedDomHost revoked(dom_module, nullptr);
    PyRef result(budget.RaiseIfStopped() ? nullptr : operation(&bridge));
    capture.result.success = result != nullptr;
    if (!result) {
      RecordError(capture.result, budget);
    } else {
      PyErr_Clear();
    }
  }
  budget.Finish();
  PyEval_SetTrace(nullptr, nullptr);
  capture.active = false;
  RecordStop(capture.result, budget);
  PyErr_Clear();
  PyEval_SaveThread();
  return std::move(capture.result);
}

PythonContext::PythonContext(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

std::unique_ptr<PythonContext> PythonContext::Create(std::string* error) {
  return CreateWithClock(error, base::DefaultTickClock::GetInstance());
}

std::unique_ptr<PythonContext> PythonContext::CreateForTesting(
    std::string* error,
    const base::TickClock* clock) {
  CHECK(clock);
  return CreateWithClock(error, clock);
}

std::unique_ptr<PythonContext> PythonContext::CreateWithClock(
    std::string* error,
    const base::TickClock* clock) {
  CHECK(error);
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  if (runtime.executing) {
    *error = "Nested Python entry is not supported";
    return nullptr;
  }
  base::AutoReset<bool> executing(&runtime.executing, true);
  if (!runtime.Initialize(error)) {
    return nullptr;
  }
  PyEval_RestoreThread(runtime.main);
  CHECK_NE(runtime.next_context_id, std::numeric_limits<uint64_t>::max());
  auto impl = std::make_unique<Impl>(clock, ++runtime.next_context_id);
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
    impl->dom_module = PyImport_ImportModule("dom");
    if (host && impl->dom_module) {
      InitializeDomCallbacks(impl->dom_module, impl->context_id);
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
    if (setup_ok) {
      impl->trace_token =
          PyCapsule_New(&impl->capture.budget, kBudgetCapsuleName, nullptr);
      setup_ok = impl->trace_token != nullptr;
    }
    if (!setup_ok) {
      *error = TakeError();
    }
  }
  if (!setup_ok) {
    impl->capture.active = false;
    Py_XDECREF(impl->globals);
    Py_XDECREF(impl->dom_module);
    Py_XDECREF(impl->trace_token);
    Py_EndInterpreter(impl->thread);
    return nullptr;
  }
  PyEval_SaveThread();
  ++runtime.statistics.contexts_created;
  ++runtime.statistics.live_contexts;
  return std::unique_ptr<PythonContext>(new PythonContext(std::move(impl)));
}

PythonContext::~PythonContext() {
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  CHECK(!runtime.executing);
  base::AutoReset<bool> executing(&runtime.executing, true);
  PyEval_RestoreThread(impl_->thread);
  impl_->capture.active = false;
  SetDomHost(impl_->dom_module, nullptr);
  impl_->capture.budget.BeginCleanup();
  PyEval_SetTrace(BudgetTrace, impl_->trace_token);
  ClearDomCallbacks(impl_->dom_module);
  Py_DECREF(impl_->globals);
  Py_DECREF(impl_->dom_module);
  // The thread trace owns the remaining reference while finalization runs.
  Py_DECREF(impl_->trace_token);
  Py_EndInterpreter(impl_->thread);
  ++runtime.statistics.contexts_destroyed;
  --runtime.statistics.live_contexts;
  runtime.statistics.cleanup_budget_exhaustions +=
      impl_->capture.budget.cleanup_exhausted();
}

ExecutionResult PythonContext::Execute(std::string_view source,
                                       std::string_view filename,
                                       DomHost* dom_host) {
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  if (source.size() > kSourceLimit || source.contains('\0') ||
      filename.contains('\0') || filename.size() > 2048 ||
      !base::IsStringUTF8(source) || !base::IsStringUTF8(filename)) {
    ExecutionResult result;
    result.error = "Source/filename must be bounded UTF-8 without NUL";
    return result;
  }
  return impl_->Run(dom_host, [&](DomHost* bridge) -> PyObject* {
    auto& budget = impl_->capture.budget;
    const std::string input(source), name(filename);
    PyRef code(
        budget.RaiseIfStopped()
            ? nullptr
            : Py_CompileString(input.c_str(), name.c_str(), Py_file_input));
    PyObject* result = nullptr;
    if (code && !budget.RaiseIfStopped()) {
      ScopedDomHost capability(impl_->dom_module, bridge);
      result = PyEval_EvalCode(code.get(), impl_->globals, impl_->globals);
    }
    return result;
  });
}

CallbackRegistrationResult PythonContext::RegisterCallback(
    std::string_view global_name,
    ExecutionKind kind) {
  CallbackRegistrationResult registration;
  Runtime::Get().CheckThread();
  if (global_name.empty() || global_name.size() > 2048 ||
      global_name.contains('\0') || !base::IsStringUTF8(global_name)) {
    registration.result.error =
        "Callback name must be bounded UTF-8 without NUL";
    return registration;
  }
  registration.result = impl_->Run(nullptr, [&](DomHost*) -> PyObject* {
    PyRef name(
        PyUnicode_FromStringAndSize(global_name.data(), global_name.size()));
    PyObject* callback = nullptr;
    if (!name || PyDict_GetItemRef(impl_->globals, name.get(), &callback) < 0) {
      return nullptr;
    }
    PyRef owned(callback);
    if (!owned) {
      PyErr_SetString(PyExc_NameError, "The callback global was not found");
      return nullptr;
    }
    registration.handle =
        RegisterDomCallback(impl_->dom_module, owned.get(),
                            kind == ExecutionKind::kAnimationFrame, kind);
    return registration.handle.id ? Py_NewRef(Py_None) : nullptr;
  });
  if (!registration.result.success) {
    registration.handle = {};
  }
  return registration;
}

ExecutionResult PythonContext::InvokeCallback(CallbackHandle handle,
                                              const CallbackEvent& event,
                                              DomHost* dom_host) {
  return impl_->Call(handle, ExecutionKind::kCallback, dom_host, [&] {
    return CreateDomCallbackEvent(impl_->dom_module, event);
  });
}

ExecutionResult PythonContext::InvokeFrameCallback(CallbackHandle handle,
                                                   double timestamp_ms,
                                                   DomHost* dom_host) {
  Runtime::Get().CheckThread();
  if (!std::isfinite(timestamp_ms) || timestamp_ms < 0) {
    ExecutionResult result;
    result.error = "Animation timestamp must be finite and nonnegative";
    return result;
  }
  return impl_->Call(handle, ExecutionKind::kAnimationFrame, dom_host,
                     [&] { return PyFloat_FromDouble(timestamp_ms); });
}

ExecutionResult PythonContext::Impl::Call(
    CallbackHandle handle,
    ExecutionKind kind,
    DomHost* dom_host,
    base::FunctionRef<PyObject*()> make_argument) {
  return Run(
      dom_host,
      [&](DomHost* bridge) -> PyObject* {
        PyRef callback(GetDomCallback(dom_module, handle, kind));
        if (!callback) {
          return nullptr;
        }
        PyRef argument(make_argument());
        if (!argument) {
          return nullptr;
        }
        PyRef result;
        {
          ScopedDomHost capability(dom_module, bridge);
          result.reset(PyObject_CallOneArg(callback.get(), argument.get()));
        }
        if (result && (PyCoro_CheckExact(result.get()) ||
                       PyGen_CheckExact(result.get()) ||
                       PyAsyncGen_CheckExact(result.get()))) {
          if (!PyAsyncGen_CheckExact(result.get())) {
            PyRef closed(PyObject_CallMethod(result.get(), "close", nullptr));
            if (!closed) {
              return nullptr;
            }
          }
          PyErr_SetString(PyExc_TypeError,
                          "Callback results must be synchronous");
          return nullptr;
        }
        // Return values are ignored and released with the DOM capability
        // revoked.
        return result ? Py_NewRef(Py_None) : nullptr;
      },
      kind);
}

bool PythonContext::CanInvokeCallback() {
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  return !runtime.executing && impl_->capture.budget.CanBeginCallback();
}

bool PythonContext::CanInvokeFrameCallback() {
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  return !runtime.executing &&
         impl_->capture.budget.CanBeginCallback(ExecutionKind::kAnimationFrame);
}

ExecutionResult PythonContext::RemoveCallback(CallbackHandle handle) {
  auto& runtime = Runtime::Get();
  runtime.CheckThread();
  auto remove = [&](DomHost*) -> PyObject* {
    return RemoveDomCallback(impl_->dom_module, handle) ? Py_NewRef(Py_None)
                                                        : nullptr;
  };
  if (!runtime.executing || runtime.active_context_id != impl_->context_id) {
    return impl_->Run(nullptr, remove);
  }
  // A host can revoke the current callback while its interpreter is already
  // entered. This mutates only the registry; it does not acquire a GIL, switch
  // interpreters or run a nested callback. Decrefs cannot inherit DOM access.
  ScopedDomHost revoked(impl_->dom_module, nullptr);
  auto& budget = impl_->capture.budget;
  PyRef result(budget.RaiseIfStopped() ? nullptr : remove(nullptr));
  ExecutionResult execution;
  execution.success = result != nullptr;
  if (!result) {
    RecordError(execution, budget);
  }
  budget.Check();
  RecordStop(execution, budget);
  PyErr_Clear();
  return execution;
}

RuntimeStatistics GetRuntimeStatisticsForTesting() {
  Runtime::Get().CheckThread();
  return Runtime::Get().statistics;
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
      exception.error.contains("<phase-one-error>") && !output.success &&
      output.stop_reason == StopReason::kOutput && output.output_truncated &&
      output.stdout_text.size() <= kOutputLimit &&
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

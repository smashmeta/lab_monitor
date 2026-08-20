#pragma once

/// Marks a class whose *data* symbols have to cross the DLL boundary.
///
/// `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` has CMake generate a .def file listing
/// every function a DLL defines, which covers ordinary code completely — no
/// other library here needs an export macro at all. What it does not cover is
/// global **data**, and every `Q_OBJECT` class has exactly one such symbol: the
/// `static const QMetaObject staticMetaObject` that moc generates.
///
/// Anything outside lm_ui that reaches for it — `qobject_cast`, a `QSignalSpy`
/// constructed from a signal, `QMetaMethod::fromSignal` — then fails to link
/// with an unresolved external. That is not a hypothetical: switching these
/// libraries from STATIC to SHARED broke exactly two classes this way,
/// `SampleCoalescer` and `TokenEdit`, because those happened to be the two the
/// applications and tests used through the meta-object system. Every other
/// `Q_OBJECT` class here is one `qobject_cast` away from the same failure, so
/// all of them carry the macro rather than only the two that broke first.
///
/// It belongs on `Q_OBJECT` classes and nowhere else. Plain functions and value
/// types are already in the .def file, and annotating them would only add
/// duplicate-export noise.
#if defined(_WIN32)
#  if defined(lm_ui_EXPORTS)  // defined by CMake while building lm_ui itself
#    define LM_UI_EXPORT __declspec(dllexport)
#  else
#    define LM_UI_EXPORT __declspec(dllimport)
#  endif
#else
#  define LM_UI_EXPORT __attribute__((visibility("default")))
#endif

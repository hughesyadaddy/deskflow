#include <QtCore/qbytearray.h>
#include <QtCore/qglobal.h>

namespace {

// GUI-linked test binaries otherwise register with the Dock for their whole
// lifetime; a suite run floods it with dozens of "exec" icons. Runs before
// main() so it precedes QApplication in QTEST_MAIN. A caller that sets the
// platform explicitly wins.
struct OffscreenByDefault
{
  OffscreenByDefault()
  {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
      qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
  }
} const offscreenByDefault;

} // namespace

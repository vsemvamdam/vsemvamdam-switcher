// vsemvamdam-switcher — Platform base helpers
#include "platform.h"

#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../os/win.h"

namespace vs {

// ---------------------------------------------------------------- Platform base
OpResult Platform::start_client() {
    OpResult r;
    std::string exe = exe_path();
    if (exe.empty() || !fsx::exists(exe)) {
        r.message = std::string(TR("Не найден клиент ", "Client not found: ")) + name();
        return r;
    }
    r.ok = os::launch(exe, "", fsx::parent(exe), true);
    r.message = r.ok ? name() + TR(" запускается", " is starting") : std::string(TR("Не удалось запустить ", "Failed to start ")) + name();
    return r;
}

OpResult Platform::close_client() {
    OpResult r;
    auto k = os::kill_processes(process_names(), 6000);
    r.ok = k.access_denied == 0 && !k.timed_out;
    r.needs_admin = k.access_denied > 0;
    r.message = r.ok ? name() + TR(" закрыт", " closed") : std::string(TR("Не удалось закрыть ", "Failed to close ")) + name();
    return r;
}

bool Platform::running() const { return os::any_running(process_names()); }

}  // namespace vs

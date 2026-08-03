-- Builds the ignored frontend artifact (build/frontend/views). Shared by
-- before_build AND before_install in the root xmake.lua — keep it wired into
-- both: `xmake install` does not run the build phase, and
-- tools/package.ps1 -SkipBuild relies on before_install producing the views
-- on its own. Node is a developer/build dependency; it is never required on a
-- player's machine.
import("core.project.config")

function build()
    local frontend = path.join(os.projectdir(), "frontend")
    cprint("${dim}building built-in views ..")
    local npm = os.host() == "windows" and "npm.cmd" or "npm"
    os.vrunv(npm, { "--prefix", frontend, "run", "build" })
end

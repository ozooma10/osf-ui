import("core.project.depend")
import("core.project.project")

local function copy_if_exists(source, destination)
    if os.isfile(source) then
        os.mkdir(path.directory(destination))
        os.cp(source, destination)
    end
end

function build_papyrus()
    os.execv("pwsh", { "-NoProfile", "-File", path.join(os.projectdir(), "tools", "build-papyrus.ps1") })
end

local function sync_data(target)
    local projectdir = os.projectdir()
    local installdir = target:installdir()
    local pluginsdir = path.join(installdir, "SFSE", "Plugins")
    local uidata = path.join(pluginsdir, "OSF", "UI")
    local views = path.join(uidata, "views")
    local sharedkit = path.join(projectdir, "web")

    -- Validate before removing installed views: wildcard copies allow missing sources.
    for _, name in ipairs({ "osfui.js", "osfui.css", "gamepadnav.js" }) do
        local source = path.join(sharedkit, name)
        if not os.isfile(source) then
            raise("OSF UI shared web asset is missing: " .. source)
        end
    end

    -- OSF UI owns only OSF/UI and the one osfui schema. Never clean OSF or
    -- OSF/Settings: those paths are shared with the independent dependency.
    os.rm(views)
    os.mkdir(path.join(views, "shared"))
    os.cp(path.join(sharedkit, "*"), path.join(views, "shared"))

    copy_if_exists(
        path.join(projectdir, "data", "SFSE", "Plugins", "OSF", "Settings", "schemas", "osfui.json"),
        path.join(pluginsdir, "OSF", "Settings", "schemas", "osfui.json"))

    local scripts = path.join(installdir, "Scripts")
    os.mkdir(path.join(scripts, "Source"))
    os.cp(path.join(projectdir, "build", "papyrus", "OSFUI.pex"), path.join(scripts, "OSFUI.pex"))
    os.cp(path.join(projectdir, "data", "Scripts", "Source", "OSFUI.psc"), path.join(scripts, "Source", "OSFUI.psc"))
    -- Remove only the retired scripts owned by this mod from earlier deployments.
    for _, name in ipairs({ "OSFUI_View", "OSFUI_Settings" }) do
        os.rm(path.join(scripts, name .. ".pex"))
        os.rm(path.join(scripts, "Source", name .. ".psc"))
    end
end

local function copy_host(target, required)
    local host = project.target("osfui-webview2-host")
    if not host or not os.isfile(host:targetfile()) then
        if required then raise("OSF UI WebView2 host was not built") end
        return
    end
    local bindir = path.join(target:installdir(), "SFSE", "Plugins", "OSF", "UI", "bin")
    os.mkdir(bindir)
    os.cp(host:targetfile(), path.join(bindir, "osfui_webview2_host.exe"))
    cprint("${dim}deploying osfui_webview2_host.exe to %s ..", bindir)
end

function deploy(target)
    if not (os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH")) then return end
    local projectdir = os.projectdir()
    local files = os.files(path.join(projectdir, "data", "**"))
    table.insert(files, path.join(projectdir, "build", "papyrus", "OSFUI.pex"))
    table.join2(files, os.files(path.join(projectdir, "web", "**")))
    depend.on_changed(function()
        sync_data(target)
        cprint("${dim}deploying owned OSF/UI paths and osfui schema to %s ..", target:installdir())
    end, { files = files, values = files, dependfile = target:dependfile("osfui_data_deploy") })
    copy_host(target, false)
end

function install(target)
    sync_data(target)
    copy_host(target, true)
end

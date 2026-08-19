import("core.project.depend")
import("core.project.project")

local function sync_data(target, cleanViews)
    local projectdir = os.projectdir()
    local installdir = target:installdir()
    local pluginsdir = path.join(installdir, "SFSE", "Plugins")
    local osfuidir = path.join(pluginsdir, "OSFUI")
    local deployedViews = path.join(osfuidir, "views")

    os.cp(path.join(projectdir, "data", "OSFUI"), pluginsdir)
    if cleanViews then
        os.rm(deployedViews)
    else
        os.rm(path.join(deployedViews, "shared"))
        os.rm(path.join(deployedViews, "osfui"))
    end
    os.cp(path.join(projectdir, "build", "frontend", "views"), osfuidir)
    os.cp(path.join(projectdir, "data", "Scripts"), installdir)
end

local function copy_host(target, required)
    local host = project.target("osfui-webview2-host")
    if not host or not os.isfile(host:targetfile()) then
        if required then
            raise("OSFUI WebView2 host was not built; cannot install a runnable plugin")
        end
        return
    end

    local bindir = path.join(target:installdir(), "SFSE", "Plugins", "OSFUI", "bin")
    os.mkdir(bindir)
    os.cp(host:targetfile(), path.join(bindir, "osfui_webview2_host.exe"))
    cprint("${dim}deploying osfui_webview2_host.exe to %s ..", bindir)
end

function deploy(target)
    if not (os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH")) then
        return
    end

    local projectdir = os.projectdir()
    local files = os.files(path.join(projectdir, "data", "**"))
    table.join2(files, os.files(path.join(projectdir, "build", "frontend", "views", "**")))
    depend.on_changed(function()
        sync_data(target, false)
        cprint("${dim}deploying data/OSFUI + generated views + data/Scripts to %s ..", target:installdir())
    end, {
        files = files,
        values = files,
        dependfile = target:dependfile("osfui_data_deploy")
    })
    copy_host(target, false)
end

function install(target)
    sync_data(target, true)
    copy_host(target, true)
end

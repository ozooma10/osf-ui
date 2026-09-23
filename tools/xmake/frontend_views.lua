function build()
    local frontend = path.join(os.projectdir(), "frontend")
    cprint("${dim}building shared web kit ..")
    local npm = os.host() == "windows" and "npm.cmd" or "npm"
    os.vrunv(npm, { "--prefix", frontend, "run", "build" })
end

function build_papyrus()
    os.execv("pwsh", {"-NoProfile", "-File", path.join(os.projectdir(),"tools","build-papyrus.ps1"), "-WorldScreens"})
end
function install(target)
    local root=target:installdir()
    local extension=path.join(os.projectdir(),"extensions","world-screens")
    os.mkdir(path.join(root,"Scripts","Source"))
    os.cp(path.join(extension,"data","Scripts","Source","OSFUIWorldScreens.psc"), path.join(root,"Scripts","Source","OSFUIWorldScreens.psc"))
    os.cp(path.join(os.projectdir(),"build","papyrus","OSFUIWorldScreens.pex"), path.join(root,"Scripts","OSFUIWorldScreens.pex"))
    local docs=path.join(root,"SFSE","Plugins","OSF","WorldScreens")
    os.mkdir(docs)
    os.cp(path.join(extension,"README.md"), path.join(docs,"README.md"))
    local schemas=path.join(root,"SFSE","Plugins","OSF","Settings","schemas")
    os.mkdir(schemas)
    os.cp(path.join(extension,"data","SFSE","Plugins","OSF","Settings","schemas","osfuiworldscreens.json"),
        path.join(schemas,"osfuiworldscreens.json"))
    local views=path.join(root,"SFSE","Plugins","OSF","UI","views","osfui.browser")
    os.mkdir(views)
    os.cp(path.join(extension,"data","SFSE","Plugins","OSF","UI","views","osfui.browser","tv"),views)
    os.mkdir(path.join(root,"textures","osfui","feeds"))
    os.cp(path.join(extension,"data","textures","osfui","feeds","*"),path.join(root,"textures","osfui","feeds"))
    os.mkdir(path.join(root,"materials","osfui","screens"))
    os.cp(path.join(extension,"data","materials","osfui","screens","*"),path.join(root,"materials","osfui","screens"))
    for _, name in ipairs({"LICENSE", "EXCEPTIONS"}) do
        os.cp(path.join(os.projectdir(),name),path.join(docs,name))
    end
end

# create-osfui

Create a directly deployable web-view starter for [OSF UI](https://github.com/ozooma10/osf-ui).

```sh
npm create osfui@latest my-osfui-mod -- --yes --mod-id acme.widgets --view main --surface menu --integration papyrus
```

The Papyrus and native presets both generate plain HTML, CSS, and JavaScript beneath
`mod/Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-id>/`. No frontend framework or
dependency install is added. The matching `OSFUI_View` API declarations are copied
into the generated project.

Settings scaffolding is intentionally not included. Author settings against the
independent [OSF Settings](https://github.com/ozooma10/osf-settings) SDK and explicitly
forward only values that a web view needs.

Options:

```text
--mod-id <id>
--view <id>
--surface menu
--integration <papyrus|native>
--yes
--no-install
--help
```

Generated views require OSF UI 2.x; OSF UI itself requires OSF Settings 1.x.

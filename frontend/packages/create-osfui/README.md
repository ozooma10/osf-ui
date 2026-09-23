# create-osfui

Scaffold a deployable web view for [OSF UI](https://github.com/ozooma10/osf-ui).

```sh
npm create osfui@latest my-osfui-mod -- --yes --mod-id acme.widgets --view main --surface menu --integration papyrus
```

| Option | Values |
| --- | --- |
| `--mod-id <id>` | Lowercase OSF mod ID |
| `--view <id>` | View ID |
| `--surface` | `menu` |
| `--integration` | `papyrus` or `native` |
| `--yes` | Skip prompts |

Both presets emit plain HTML, CSS, and JavaScript at `mod/Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-id>/` plus the `OSFUI` type declarations. No framework, no install step.

Settings are not scaffolded: use the [OSF Settings](https://github.com/ozooma10/osf-settings-slim) SDK and forward only the values the page needs. Generated views need OSF UI 2.x.

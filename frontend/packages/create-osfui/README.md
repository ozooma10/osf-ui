# create-osfui

Create a directly deployable [OSF UI](https://github.com/ozooma10/osf-ui) starter for a Starfield mod.

Menu starters use ordinary `index.html`, `style.css`, and `main.js` files. They do not add a frontend framework, a web build step, or project dependencies. The bootstrapper also includes the matching Papyrus declarations or native C++ headers so the generated project is self-contained.

## Quick start

Node.js 20.19 or newer is required.

```sh
npm create osfui@latest my-osfui-mod
```

The interactive prompts choose the mod ID, starter type, view name, and integration. To create a project non-interactively:

```sh
npm create osfui@latest my-osfui-mod -- --yes --mod-id acme.widgets --view main --surface menu --integration papyrus
```

The destination must be empty. The bootstrapper never installs dependencies in the generated project.

## Starters

| Starter | Integration | Generated project |
| --- | --- | --- |
| Menu | Papyrus | Plain HTML/CSS/JavaScript view, settings schema, Papyrus script, declarations, and build/deploy script |
| Menu | Native plugin | Plain HTML/CSS/JavaScript view, settings and localization examples, C++ plugin source, OSF UI headers, and XMake project |
| Settings only | Papyrus | Settings schema, Papyrus hotkey handler, declarations, and build/deploy script; no menu view |

The generated `README.md` contains the build and deployment instructions for the selected starter.

## Options

```text
npm create osfui@latest [directory] -- [options]

--mod-id <id>                 Settings and bridge namespace
--view <id>                   Menu view ID (default: main)
--surface <menu|settings>     Starter type (default: menu)
--integration <papyrus|native>
--yes                         Use defaults for missing values
--no-install                  Compatibility no-op; starters install nothing
--help                        Show command usage
```

The settings-only starter is Papyrus-only. A native integration creates a menu starter.

## Runtime compatibility

Generated manifests target OSF UI 2.0.0. Install a compatible OSF UI runtime in the game before deploying the generated mod.

See the [OSF UI repository](https://github.com/ozooma10/osf-ui) for runtime installation and authoring documentation.

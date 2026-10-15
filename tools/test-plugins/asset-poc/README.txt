Test plugin for cooked plugin content (docs/custom-assets-recipe.md). content/ is its UE 5.8.3 project:
  POC_STEP=1|2 UnrealEditor-Cmd content/asset_poc.uproject -run=pythonscript -script=<abs>/content/make_assets.py
  python tools/make_mod_container.py content/asset_poc.uproject <plugin folder>/assets
Containers (assets/*) are build output and are never committed.

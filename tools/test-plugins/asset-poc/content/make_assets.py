# Run with the editor's pythonscript commandlet. POC_STEP=1: magenta, Tint -> Base Color + Emissive.
# POC_STEP=2: a different shader graph (Fresnel rim: Emissive = Tint * Fresnel * 6, Base Color = Tint * 0.15), cyan.
import os
import unreal

step = int(os.environ.get("POC_STEP", "1"))
folder = "/Game/Mods/asset_poc"
lib = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

for name in ("MI_PoC", "M_PoCParent"):
    if lib.does_asset_exist(f"{folder}/{name}"):
        lib.delete_asset(f"{folder}/{name}")

mat = tools.create_asset("M_PoCParent", folder, unreal.Material, unreal.MaterialFactoryNew())
tint = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -800, 0)
tint.set_editor_property("parameter_name", "Tint")
tint.set_editor_property("default_value", unreal.LinearColor(0.1, 0.1, 0.1, 1))
if step == 1:
    mel.connect_material_property(tint, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(tint, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    colour = unreal.LinearColor(1.0, 0.0, 0.85, 1.0)
else:
    fres = mel.create_material_expression(mat, unreal.MaterialExpressionFresnel, -600, 200)
    fres.set_editor_property("exponent", 2.5)
    k = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -600, 350)
    k.set_editor_property("r", 6.0)
    m1 = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -400, 200)
    mel.connect_material_expressions(tint, "RGB", m1, "A")
    mel.connect_material_expressions(fres, "", m1, "B")
    m2 = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -200, 200)
    mel.connect_material_expressions(m1, "", m2, "A")
    mel.connect_material_expressions(k, "", m2, "B")
    mel.connect_material_property(m2, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    dim = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -400, 0)
    dim.set_editor_property("const_b", 0.15)
    mel.connect_material_expressions(tint, "RGB", dim, "A")
    mel.connect_material_property(dim, "", unreal.MaterialProperty.MP_BASE_COLOR)
    colour = unreal.LinearColor(0.0, 0.9, 1.0, 1.0)
mel.recompile_material(mat)

mic = tools.create_asset("MI_PoC", folder, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
mic.set_editor_property("parent", mat)
mel.set_material_instance_vector_parameter_value(mic, "Tint", colour)
lib.save_asset(mat.get_path_name())
lib.save_asset(mic.get_path_name())
print("POC_ASSETS_STEP", step)

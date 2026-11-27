"""Real UE editor regression, using only a disposable project and the latest package.

Run from any directory: python tests/ue_bridge_integration.py [--config path.ini]
No third-party Python packages, compilation, deployment, or asset saves are involved.
Evidence remains under ue_plugins/build/integration-tests/<unique run>/.
"""

from __future__ import annotations

import argparse
import collections
import configparser
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time
import traceback
import uuid
from contextlib import contextmanager


ROOT = Path(__file__).resolve().parents[1]
MAX_FRAME = 256 * 1024 * 1024
SESSION_SCRIPT = r'''
import json
import pathlib
import struct
import traceback
import unreal

directory = pathlib.Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()))
session_id = json.loads((directory / "qa-session.json").read_text())["session_id"]
request_file = directory / "test-command.json"

def bridge_test_tick(delta):
    if not request_file.exists():
        return
    data = json.loads(request_file.read_text(encoding="utf-8"))
    request_file.unlink()
    if data.get("session_id") != session_id:
        return
    response = {"seq": data["seq"], "ok": True}
    try:
        command = data["command"]
        if command in ("UNDO", "REDO"):
            unreal.SystemLibrary.execute_console_command(None, "TRANSACTION " + command)
        elif command == "INSPECT":
            response["classes"] = {}
            for path in data["paths"]:
                obj = unreal.find_object(None, path)
                response["classes"][path] = obj.get_class().get_name() if obj else None
        elif command in ("COPY_TEST_SKELETON", "COPY_TEST_STATIC"):
            skeleton = command == "COPY_TEST_SKELETON"
            source = unreal.find_object(None, data["source"], type=unreal.Skeleton if skeleton else unreal.StaticMesh)
            assert source, data["source"]
            destination = data["destination"]
            folder, name = destination.rsplit("/", 1)
            assert folder.startswith("/Game/Vehicle/SportsCar/")
            assert folder.endswith("/Skeleton" if skeleton else "/StaticMesh")
            if skeleton:
                assert name.startswith("SKEL_")
            duplicate = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, folder, source)
            assert duplicate and duplicate.get_path_name() == destination + "." + name
        elif command == "MESH_SKELETON":
            mesh = unreal.find_object(None, data["mesh"], type=unreal.SkeletalMesh)
            response["skeleton"] = mesh.get_editor_property("skeleton").get_path_name()
        elif command == "VEHICLE_DRIVING_DEFAULTS":
            blueprint = unreal.load_asset(data["blueprint"])
            default_car = unreal.get_default_object(blueprint.generated_class())
            response["default_ai_disabled"] = default_car.get_editor_property("auto_possess_ai") == unreal.AutoPossessAI.DISABLED
            actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
            car = actors.spawn_actor_from_class(blueprint.generated_class(), unreal.Vector(), transient=True)
            try:
                response["ai_disabled"] = car.get_editor_property("auto_possess_ai") == unreal.AutoPossessAI.DISABLED
                response["orbit_cameras"] = len(car.get_components_by_class(unreal.FGearOrbitCamera))
                response["axles"] = {axle.get_name(): [axle.get_editor_property("mMaxSteerAngle"),
                                                       axle.get_editor_property("mHasHandbrake")]
                                     for axle in car.get_components_by_class(unreal.FGearAxle)}
            finally:
                actors.destroy_actor(car)
        elif command == "SET_PHYSICS_BINDINGS":
            mesh = unreal.load_asset(data["mesh"])
            physics = unreal.load_asset(data["physics"])
            assert mesh.get_outermost().get_path_name().startswith("/Game/Vehicle/SportsCar/PhysicsQA")
            assert physics.get_outermost().get_path_name().startswith("/Game/Vehicle/SportsCar/PhysicsQA")
            linked = unreal.load_asset(data["mesh_physics"]) if data["mesh_physics"] else None
            mesh.set_editor_property("physics_asset", linked)
            # Use the reflected soft-property setter for this editor-only fixture field.
            preview = unreal.load_asset(data["physics_preview"])
            unreal.get_default_object(unreal.SystemLibrary).call_method(
                "SetSoftObjectPropertyByName", (physics, "PreviewSkeletalMesh", preview))
        elif command == "SKELETAL_COMPONENT_BOUNDS":
            mesh = unreal.load_asset(data["mesh"])
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            # Spawn through gameplay APIs so measuring the bounds leaves the editor undo stack intact.
            gameplay = unreal.get_default_object(unreal.GameplayStatics)
            transform = unreal.Transform()
            actor = gameplay.call_method("BeginDeferredActorSpawnFromClass", (world, unreal.SkeletalMeshActor, transform))
            try:
                gameplay.call_method("FinishSpawningActor", (actor, transform))
                component = actor.skeletal_mesh_component
                component.set_skinned_asset_and_update(mesh)
                origin, extent, _ = unreal.SystemLibrary.get_component_bounds(component)
                response["bounds"] = [[v.x, v.y, v.z] for v in (origin, extent)]
                imported = mesh.get_bounds()
                response["mesh_bounds"] = [[v.x, v.y, v.z] for v in (imported.origin, imported.box_extent)]
            finally:
                actor.destroy_actor()
        elif command == "PHYSICS_BINDINGS":
            mesh = unreal.load_asset(data["mesh"])
            physics = unreal.load_asset(data["physics"])
            linked = mesh.get_editor_property("physics_asset")
            response["mesh_physics"] = linked.get_outermost().get_path_name() if linked else None
            # PreviewSkeletalMesh is editor-only and searchable, but is not an editable Python property.
            preview_path = unreal.AssetRegistryHelpers.create_asset_data(physics).get_tag_value("PreviewSkeletalMesh")
            preview = unreal.load_asset(preview_path) if preview_path and preview_path != "None" else None
            response["physics_preview"] = preview.get_outermost().get_path_name() if preview else None
            component = unreal.SkeletalMeshComponent()
            component.set_skinned_asset_and_update(preview or mesh)
            response["body_index"] = component.get_bone_index("Body")
            response["body_parent"] = str(component.get_parent_bone("Body"))
            bodies = {str(body.get_editor_property("bone_name")): body
                      for body in unreal.ObjectIterator(unreal.BodySetup) if body.get_outer() == physics}
            response["hulls"] = {name: len(body.get_editor_property("agg_geom").get_editor_property("convex_elems"))
                                 for name, body in bodies.items()}
            body = bodies["Body"]
            response["body_simulated"] = body.get_editor_property("physics_type") == unreal.PhysicsType.PHYS_TYPE_SIMULATED
            response["simple_as_complex"] = (body.get_editor_property("collision_trace_flag") ==
                                             unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)
        elif command == "MOVE_TEST_CONVEX":
            physics = unreal.load_asset(data["physics"])
            assert physics.get_outermost().get_path_name().startswith("/Game/Vehicle/SportsCar/PhysicsQA")
            body = next(b for b in unreal.ObjectIterator(unreal.BodySetup)
                        if b.get_outer() == physics and str(b.get_editor_property("bone_name")) == "Body")
            geometry = body.get_editor_property("agg_geom")
            hulls = geometry.get_editor_property("convex_elems")
            assert len(hulls) == 1
            elem = hulls[0]
            # FKConvexElem.Transform is reflected but has no public Python setter.
            assert elem.import_text("(Transform=(Translation=(X=0,Y=0,Z=3)))")
            hulls[0] = elem
            geometry.set_editor_property("convex_elems", hulls)
            body.set_editor_property("agg_geom", geometry)
        elif command == "TEXTURE_FIXTURES":
            template = unreal.load_asset("/Engine/EngineResources/DefaultTexture")
            assert isinstance(template, unreal.Texture2D)
            tools = unreal.AssetToolsHelpers.get_asset_tools()
            for path in data["packages"]:
                folder, name = path.rsplit("/", 1)
                assert folder.startswith("/Game/Vehicle/")
                texture = tools.duplicate_asset(name, folder, template)
                assert texture and texture.get_outermost().get_path_name() == path
        elif command == "IMPORT_TEXTURES":
            tasks = []
            source = directory / "SourceTextures"
            source.mkdir(exist_ok=True)
            header = bytes([0, 0, 2]) + bytes(9) + struct.pack("<HHBB", 4, 4, 24, 32)
            for path in data["packages"]:
                folder, name = path.rsplit("/", 1)
                assert folder.startswith("/Game/Vehicle/")
                filename = source / (name + ".tga")
                filename.write_bytes(header + bytes([127, 127, 255]) * 16)
                task = unreal.AssetImportTask()
                task.set_editor_property("filename", str(filename))
                task.set_editor_property("destination_path", folder)
                task.set_editor_property("automated", True)
                task.set_editor_property("save", False)
                tasks.append(task)
            unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
            for task, path in zip(tasks, data["packages"]):
                assert any(isinstance(obj, unreal.Texture2D) and obj.get_outermost().get_path_name() == path
                           for obj in task.get_objects()), path
        elif command == "MATERIAL_FIXTURE":
            folder, name = data["package"].rsplit("/", 1)
            factory, asset_class = {
                "parent": (unreal.MaterialFactoryNew, unreal.Material),
                "instance": (unreal.MaterialInstanceConstantFactoryNew, unreal.MaterialInstanceConstant),
                "conflict": (unreal.MaterialFunctionFactoryNew, unreal.MaterialFunction),
            }[data["kind"]]
            obj = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, asset_class, factory())
            assert obj, data["package"]
            editing = unreal.MaterialEditingLibrary
            if data["kind"] == "parent":
                scalar = editing.create_material_expression(obj, unreal.MaterialExpressionScalarParameter)
                scalar.set_editor_property("parameter_name", "BridgeWear")
                editing.connect_material_property(scalar, "", unreal.MaterialProperty.MP_ROUGHNESS)
                if data.get("texture_parameters"):
                    default = unreal.load_asset("/Engine/EngineResources/DefaultTexture")
                    for parameter, prop in (("Color_Tex", unreal.MaterialProperty.MP_BASE_COLOR),
                                            ("M_Tex", unreal.MaterialProperty.MP_METALLIC),
                                            ("Normal_Tex", unreal.MaterialProperty.MP_NORMAL)):
                        expr = editing.create_material_expression(obj, unreal.MaterialExpressionTextureSampleParameter2D)
                        expr.set_editor_property("parameter_name", parameter)
                        expr.set_editor_property("texture", default)
                        assert editing.connect_material_property(expr, "R" if parameter == "M_Tex" else "RGB", prop)
                editing.recompile_material(obj)
            elif data["kind"] == "instance":
                parent = unreal.find_object(None, data["parent"] + "." + data["parent"].rsplit("/", 1)[1])
                assert parent and isinstance(parent, unreal.Material)
                editing.set_material_instance_parent(obj, parent)
                value = unreal.ScalarParameterValue()
                value.set_editor_property("parameter_info", unreal.MaterialParameterInfo(name="BridgeWear"))
                value.set_editor_property("parameter_value", 0.375)
                obj.set_editor_property("scalar_parameter_values", [value])
                for parameter, path in data.get("textures", {}).items():
                    texture = unreal.load_asset(path)
                    editing.set_material_instance_texture_parameter_value(obj, parameter, texture)
                    assert editing.get_material_instance_texture_parameter_value(obj, parameter) == texture
        elif command == "EDIT_MI_TEXTURE":
            obj = unreal.load_asset(data["instance"])
            texture = unreal.load_asset(data["texture"])
            unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(obj, data["parameter"], texture)
            assert unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(obj, data["parameter"]) == texture
        elif command == "MATERIAL_BINDINGS":
            response["meshes"], response["instances"] = {}, {}
            for path, lod_count in data["meshes"].items():
                mesh = unreal.load_asset(path)
                static = isinstance(mesh, unreal.StaticMesh)
                slots = mesh.get_editor_property("static_materials" if static else "materials")
                editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem if static else unreal.SkeletalMeshEditorSubsystem)
                names = [str(slot.get_editor_property("material_slot_name")) for slot in slots]
                sections = []
                for lod in range(lod_count):
                    count = mesh.get_num_sections(lod) if static else editor.get_num_sections(mesh, lod)
                    indices = [editor.get_lod_material_slot(mesh, lod, s) for s in range(count)]
                    assert all(0 <= index < len(names) for index in indices), (path, lod, indices, names)
                    sections.append([names[index] for index in indices])
                bindings = {}
                for name, slot in zip(names, slots):
                    material = slot.get_editor_property("material_interface")
                    bindings[name] = material.get_outermost().get_path_name() if material else None
                response["meshes"][path] = {"bindings": bindings, "sections": sections}
            for path in data["instances"]:
                obj = unreal.find_object(None, path + "." + path.rsplit("/", 1)[1])
                if obj:
                    parent = obj.get_editor_property("parent")
                    response["instances"][path] = {
                        "class": obj.get_class().get_name(),
                        "parent": parent.get_outermost().get_path_name() if parent else None,
                        "parameters": {str(v.get_editor_property("parameter_info").get_editor_property("name")):
                                       v.get_editor_property("parameter_value") for v in obj.get_editor_property("scalar_parameter_values")},
                        "textures": {str(v.get_editor_property("parameter_info").get_editor_property("name")):
                                     v.get_editor_property("parameter_value").get_outermost().get_path_name()
                                     for v in obj.get_editor_property("texture_parameter_values")},
                    }
                    if data.get("check_effective_textures"):
                        effective = {}
                        for param in ("Color_Tex", "M_Tex", "Normal_Tex"):
                            tex = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(obj, param)
                            effective[param] = tex.get_outermost().get_path_name() if tex else None
                        response["instances"][path]["effective"] = effective
                        pending = {}
                        for user_data in obj.get_editor_property("asset_user_data"):
                            if user_data and user_data.get_class().get_name() == "WyPendingVehicleTextures":
                                for param, soft_path in user_data.get_editor_property("PendingTextures").items():
                                    pending[str(param)] = str(soft_path.export_text())
                        response["instances"][path]["pending"] = pending
                else:
                    response["instances"][path] = None
        elif command == "EDIT_MESH_SLOTS":
            mesh = unreal.load_asset(data["mesh"])
            slot_property = "static_materials" if isinstance(mesh, unreal.StaticMesh) else "materials"
            slots = mesh.get_editor_property(slot_property)
            names = [str(s.get_editor_property("material_slot_name")) for s in slots]
            if "rename" in data:
                source = names.index(data["rename"])
                renamed = slots[source]
                renamed.set_editor_property("material_slot_name", data["new_name"])
                if "assign" in data:
                    renamed.set_editor_property("material_interface", unreal.load_asset(data["assign"]))
                slots[source] = renamed
            else:
                source = len(slots)
                slots.append(unreal.StaticMaterial(material_slot_name=data["new_name"]))
            mesh.set_editor_property(slot_property, slots)
            if "target" in data:
                target = names.index(data["target"])
                editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
                lod = data["lod"]
                for section in range(mesh.get_num_sections(lod)):
                    if editor.get_lod_material_slot(mesh, lod, section) == target:
                        editor.set_lod_material_slot(mesh, source, lod, section)
                        break
                else:
                    raise AssertionError("Expected a section using " + data["target"])
        elif command != "EXIT":
            raise ValueError("Unknown test command")
    except Exception:
        response = {"seq": data["seq"], "ok": False, "error": traceback.format_exc()}
    temporary = directory / "test-ack.tmp"
    temporary.write_text(json.dumps(response), encoding="utf-8")
    temporary.replace(directory / ("test-ack-" + data["seq"] + ".json"))
    if data.get("command") == "EXIT":
        unreal.SystemLibrary.quit_editor()

_bridge_test_handle = unreal.register_slate_post_tick_callback(bridge_test_tick)
(directory / "python-ready").write_text(session_id, encoding="utf-8")
'''


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def configured_path(config, section, key, ini):
    value = os.path.expandvars(config.get(section, key, fallback="").strip())
    if not value:
        raise ValueError(f"{section}.{key} is missing in {ini}")
    path = Path(value)
    return (ini.parent / path).resolve() if not path.is_absolute() else path.resolve()


def resolve_environment(ini):
    config = configparser.ConfigParser(interpolation=None)
    with ini.open(encoding="utf-8-sig") as handle:
        config.read_file(handle)
    engine = configured_path(config, "UE", "EnginePath", ini)
    configured_project = configured_path(config, "UE", "ProjectPath", ini)
    if configured_project.is_dir():
        candidates = list(configured_project.glob("*.uproject"))
        check(len(candidates) == 1, "Configured project directory must contain exactly one .uproject")
        configured_project = candidates[0]
    check(configured_project.is_file() and configured_project.suffix.lower() == ".uproject", "Configured UE project is invalid")
    editor = engine / "Engine/Binaries/Win64/UnrealEditor.exe"
    check(editor.is_file(), f"UnrealEditor.exe not found under configured engine: {engine}")
    # The user project is validated only. No files are created, copied, or loaded there.
    return editor, configured_project


def near(actual, expected, path="value", tolerance=1e-4):
    if isinstance(expected, dict):
        check(isinstance(actual, dict), f"{path}: expected object")
        for key, value in expected.items():
            check(key in actual, f"{path}.{key}: missing")
            near(actual[key], value, f"{path}.{key}", tolerance)
    elif isinstance(expected, list):
        check(isinstance(actual, list) and len(actual) == len(expected), f"{path}: array length differs")
        for index, value in enumerate(expected):
            near(actual[index], value, f"{path}[{index}]", tolerance)
    elif isinstance(expected, (int, float)):
        check(isinstance(actual, (int, float)) and math.isfinite(actual) and abs(actual - expected) <= tolerance,
              f"{path}: {actual!r} != {expected!r}")
    else:
        check(actual == expected, f"{path}: {actual!r} != {expected!r}")


def item(kind, name, lod, data):
    return {"id": name, "name": name, "kind": kind, "lod": lod, "data": data}


def mesh(name, kind="skeletalPart", lod=0, offset=(0, 0, 0), triangle=False, seed=0):
    points = [[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0]]
    faces = [[0, 1, 2], [0, 2, 3]]
    if triangle:
        points, faces = points[:3], faces[:1]
    points = [[x + d for x, d in zip(point, offset)] for point in points]
    corners = len(faces) * 3
    normals, tangents = [], []
    for corner in range(corners):
        angle = (corner + seed) * 0.31
        normal = [0.2 * math.cos(angle), 0.2 * math.sin(angle), math.sqrt(0.96)]
        length = math.hypot(normal[2], normal[0])
        normals.append(normal)
        tangents.append([normal[2] / length, 0, -normal[0] / length, -1 if corner % 2 else 1])
    data = {
        "positions": points, "triangles": faces,
        "uvs": [[[0.07 + seed * 0.003 + corner * 0.017, 0.11 + corner * 0.023] for corner in range(corners)],
                [[0.63 - corner * 0.019, 0.37 + seed * 0.004 + corner * 0.031] for corner in range(corners)]],
        "normals": normals, "tangents": tangents,
        "colors": [[0.1 + corner * 0.07, 0.2 + seed * 0.01, 0.8 - corner * 0.04, 0.65] for corner in range(corners)],
        "materialIds": [0] * len(faces),
    }
    if kind == "skeletalPart":
        data["weights"] = [[{"bone": "Body", "weight": 1.0}] for _ in points]
        data["weights"][1] = [{"bone": "Root", "weight": 0.25}, {"bone": "Body", "weight": 0.75}]
    return item(kind, name, lod, data)


def assert_mesh(actual, expected):
    expected_data = copy.deepcopy(expected["data"])
    expected_weights = expected_data.pop("weights", None)
    near(actual["data"], expected_data, expected["id"])
    if expected_weights is not None:
        actual_weights = actual["data"]["weights"]
        check(len(actual_weights) == len(expected_weights), "Skin vertex count differs")
        for index, influences in enumerate(expected_weights):
            near({v["bone"]: v["weight"] for v in actual_weights[index]},
                 {v["bone"]: v["weight"] for v in influences}, f"weights[{index}]")


def assert_protected(before, after, changed=()):
    check(set(before) == set(after), f"Unexpected scopes: {set(before) ^ set(after)}")
    for scope, value in before.items():
        if scope not in changed:
            check(value == after[scope], f"Protected scope changed: {scope}")


def concave_prism(name="ManualConcave", z0=0, z1=8, offset=(0, 0, 0)):
    polygon = [(0, 0), (20, 0), (20, 5), (5, 5), (5, 20), (0, 20)]
    positions = [[x + offset[0], y + offset[1], z + offset[2]] for z in (z0, z1) for x, y in polygon]
    cap = [(0, 1, 2), (0, 2, 3), (0, 3, 5), (3, 4, 5)]
    faces = [[c, b, a] for a, b, c in cap] + [[a + 6, b + 6, c + 6] for a, b, c in cap]
    for a in range(6):
        b = (a + 1) % 6
        faces += [[a, b, b + 6], [a, b + 6, a + 6]]
    return {"name": name, "positions": positions, "triangles": faces}


def corner_clipped_cube():
    # The solid spans 10 cm on each axis. One 0.005 cm bevel produces a small,
    # valid face on BodyLower independently of the hull's overall thickness.
    d = 0.005
    points = [[d, 0, 0], [0, d, 0], [0, 0, d], [10, 0, 0], [10, 10, 0],
              [0, 10, 0], [0, 0, 10], [10, 0, 10], [10, 10, 10], [0, 10, 10]]
    polygons = [[0, 1, 5, 4, 3], [2, 0, 3, 7, 6], [1, 2, 6, 9, 5],
                [3, 4, 8, 7], [4, 5, 9, 8], [6, 7, 8, 9], [0, 2, 1]]
    faces = [[face[0], face[i], face[i + 1]] for face in polygons for i in range(1, len(face) - 1)]
    return {"positions": points, "triangles": faces}


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def verify_convex(shape, source_points):
    points, faces = shape["positions"], shape["triangles"]
    check(len(points) >= 4 and len(faces) >= 4, "Convex output is not a solid")
    center = [sum(p[i] for p in points) / len(points) for i in range(3)]
    directed = collections.Counter()
    undirected = set()
    for face in faces:
        check(len(face) == 3 and len(set(face)) == 3, "Repeated triangle vertex")
        check(all(isinstance(i, int) and 0 <= i < len(points) for i in face), "Invalid convex index")
        a, b, c = [points[i] for i in face]
        normal = cross(sub(b, a), sub(c, a))
        length = math.sqrt(dot(normal, normal))
        check(length > 1e-8, "Degenerate convex triangle")
        check(dot(normal, sub(a, center)) > 1e-7, "Inward convex triangle")
        for source in source_points:
            check(dot(normal, sub(source, a)) / length < 1e-4, "Convex output excludes a source point")
        for first, second in zip(face, face[1:] + face[:1]):
            directed[(first, second)] += 1
            undirected.add(tuple(sorted((first, second))))
    check(all(count == 1 and directed[(b, a)] == 1 for (a, b), count in directed.items()), "Convex boundary is not closed and oriented")
    check(len(points) - len(undirected) + len(faces) == 2, "Convex Euler characteristic is not 2")


class EditorSession:
    def __init__(self, config, timeout):
        self.config, self.timeout = config, timeout
        self.identifier = uuid.uuid4().hex
        self.directory = ROOT / "ue_plugins/build/integration-tests" / self.identifier
        self.project = self.directory / "BridgeIntegration.uproject"
        self.process = None
        self.connection = None
        self.results = []
        self.info = {}
        self.started = time.monotonic()

    def start(self):
        editor, configured_project = resolve_environment(self.config)
        package = ROOT / "ue_plugins/build/latest/WyVehicleAssetBridge"
        dll = package / "Binaries/Win64/UnrealEditor-WyVehicleAssetBridge.dll"
        for path in (dll, package / "WyVehicleAssetBridge.uplugin"):
            check(path.is_file(), f"Latest plugin package missing: {path}; compile UE with -BuildOnly first")
        descriptor = json.loads((package / "WyVehicleAssetBridge.uplugin").read_text(encoding="utf-8-sig"))
        check(descriptor.get("EnabledByDefault") is True, "Built plugin must declare EnabledByDefault")
        self.directory.mkdir(parents=True)
        self.project.write_text(json.dumps({"FileVersion": 3, "Plugins": [
            {"Name": "PythonScriptPlugin", "Enabled": True}]}), encoding="utf-8")
        (self.directory / "Content/Vehicle/SportsCar").mkdir(parents=True)
        (self.directory / "Content/Vehicle/CommonVH").mkdir()
        python = self.directory / "Content/Python"
        python.mkdir()
        (python / "init_unreal.py").write_text(SESSION_SCRIPT, encoding="utf-8")
        config_dir = self.directory / "Config"
        config_dir.mkdir()
        (config_dir / "DefaultEditorPerProjectUserSettings.ini").write_text(
            "[/Script/UnrealEd.EditorLoadingSavingSettings]\nbAutoSaveEnable=False\n", encoding="utf-8")
        (self.directory / "qa-session.json").write_text(json.dumps({"session_id": self.identifier}), encoding="utf-8")
        shutil.copytree(package, self.directory / "Plugins/WyVehicleAssetBridge")
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        check(port != 52317, "Random port unexpectedly conflicts with reserved manual QA port; rerun")
        arguments = [str(editor), str(self.project), "-unattended", "-nosplash", "-nullrhi", "-nosound",
                     "-NoLiveCoding", "-NoSourceControl", "-NoAssetRegistryCache", f"-WyVehicleBridgePort={port}",
                     f"-abslog={self.directory / 'Editor.log'}"]
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        self.process = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        startupinfo=startup, creationflags=subprocess.CREATE_NO_WINDOW)
        self.info = {"session_id": self.identifier, "pid": self.process.pid, "port": port,
                     "project": str(self.project), "configured_project_read_only": str(configured_project),
                     "editor": str(editor), "plugin_dll_sha256": hashlib.sha256(dll.read_bytes()).hexdigest()}
        marker = package / ".wy-vehicle-asset-bridge-managed.json"
        if marker.exists():
            self.info["package_marker"] = json.loads(marker.read_text(encoding="utf-8-sig"))
        (self.directory / "session.json").write_text(json.dumps(self.info, indent=2), encoding="utf-8")
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            check(self.process.poll() is None, f"Owned editor exited with {self.process.returncode}; see Editor.log")
            try:
                self.connection = socket.create_connection(("127.0.0.1", port), timeout=1)
                self.connection.settimeout(70)
                hello = self.call({"op": "hello"})
                check(Path(hello["project"]).resolve() == self.project.resolve(), "Port belongs to a different UE project; refusing all mutations")
                check(hello.get("service") == "WyVehicleAssetBridge" and hello.get("protocol") == 1, "Unexpected service/protocol")
                check("SportsCar" in hello["types"], "Isolated vehicle type not detected")
                check("commonvh" not in {name.casefold() for name in hello["types"]}, "Shared asset folder offered as a vehicle type")
                break
            except OSError:
                if self.connection:
                    self.connection.close()
                    self.connection = None
                time.sleep(0.5)
        else:
            raise TimeoutError("Owned UE service did not start; see Editor.log")
        ready = self.directory / "python-ready"
        while not ready.exists() and time.monotonic() < deadline:
            check(self.process.poll() is None, "Owned editor exited before Python initialized")
            time.sleep(0.1)
        check(ready.exists() and ready.read_text() == self.identifier, "Editor Python helper did not initialize")
        print(f"Owned UE editor ready (PID {self.process.pid}, port {port}); evidence: {self.directory}", flush=True)

    def receive(self, count):
        data = bytearray()
        while len(data) < count:
            chunk = self.connection.recv(count - len(data))
            if not chunk:
                raise ConnectionError("UE closed the bridge connection")
            data.extend(chunk)
        return data

    def call(self, request, success=True):
        payload = json.dumps(request, ensure_ascii=False, allow_nan=False).encode("utf-8")
        check(len(payload) <= MAX_FRAME, "Test request exceeds bridge limit")
        self.connection.sendall(struct.pack(">I", len(payload)) + payload)
        size = struct.unpack(">I", self.receive(4))[0]
        check(0 < size <= MAX_FRAME, "Invalid response length")
        response = json.loads(self.receive(size))
        with (self.directory / "protocol.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps({"request": request, "response": response}, ensure_ascii=False) + "\n")
        if success:
            check(response.get("ok"), f"Bridge rejected {request.get('op')}: {response}")
        return response

    def read(self, vehicle):
        return {value["id"]: value for value in self.call({"op": "read", "vehicle": vehicle})["items"]}

    def write(self, vehicle, items, before, success=True):
        return self.call({"op": "write", "vehicle": vehicle, "items": items,
                          "expected": {value["id"]: before.get(value["id"], {}).get("revision", "") for value in items}}, success)

    def command(self, command, **arguments):
        seq = uuid.uuid4().hex
        request = {"session_id": self.identifier, "seq": seq, "command": command, **arguments}
        temporary = self.directory / "test-command.tmp"
        temporary.write_text(json.dumps(request), encoding="utf-8")
        temporary.replace(self.directory / "test-command.json")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            ack = self.directory / f"test-ack-{seq}.json"
            if ack.exists():
                response = json.loads(ack.read_text(encoding="utf-8"))
                if response.get("seq") == seq:
                    check(response["ok"], response.get("error", "Editor command failed"))
                    return response
            check(self.process.poll() is None, "Owned editor exited while waiting for test command")
            time.sleep(0.05)
        raise TimeoutError(f"Editor command timed out: {command}")

    @contextmanager
    def case(self, name):
        started = time.monotonic()
        result = {"name": name, "passed": False}
        try:
            yield
            result["passed"] = True
            print(f"PASS {name}", flush=True)
        except Exception:
            result["error"] = traceback.format_exc()
            raise
        finally:
            result["seconds"] = round(time.monotonic() - started, 3)
            self.results.append(result)

    def close(self):
        if self.connection:
            self.connection.close()
            self.connection = None
        if self.process and self.process.poll() is None:
            try:
                if (self.directory / "python-ready").exists():
                    self.command("EXIT")
                    self.process.wait(timeout=30)
            except (Exception, KeyboardInterrupt) as error:
                self.info["graceful_exit_error"] = str(error)
            if self.process.poll() is None:
                self.info["forced_exit"] = True
                # Popen retains the handle to exactly the editor started above.
                # Never enumerate/terminate other UE, Max, or shader processes.
                self.process.terminate()
                self.process.wait(timeout=20)
        if self.process:
            self.info["editor_exit_code"] = self.process.poll()


def check_blueprint_creation(session):
    name = "BlueprintQA" + uuid.uuid4().hex[:8]
    vehicle = {"type": "SportsCar", "name": name, "variant": "Track"}
    base = f"/Game/Vehicle/SportsCar/{name}/{name}_Track"
    content = session.directory / "Content/Vehicle/SportsCar" / name
    parts = ("Hub_0000", "Tire_0000", "BrakeF", "BrakeR")
    packages = [f"{base}/Blueprint/BP_{name}_Track", f"{base}/AnimBlueprint/ABP_{name}_Track",
                f"{base}/SkeletalMesh/SK_{name}", f"{base}/Skeleton/SKEL_{name}_Track",
                *(f"{base}/StaticMesh/SM_{part}" for part in parts)]
    paths = [path + "." + path.rsplit("/", 1)[1] for path in packages]
    base_bone = {"name": "Root", "parent": "", "translation": [0, 0, 0],
                 "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}
    root = item("skeleton", "Root", 0, {"bones": [base_bone, dict(base_bone, name="Body", parent="Root")]})
    bones = root["data"]["bones"]
    bones.extend(dict(base_bone, name="D_Wheel" + side, parent="Root") for side in ("FL", "FR", "BL", "BR"))
    items = [root, mesh("LOD0_Body"), *(mesh("LOD0_" + part, "staticMesh") for part in parts)]

    def write(values):
        return session.call({"op": "write", "vehicle": vehicle, "items": values,
                             "expected": {v["id"]: "" for v in values}, "createVehicleBlueprints": True}, success=False)

    def classes():
        return session.command("INSPECT", paths=paths)["classes"]

    with session.case("Base and variant blueprint names; missing wheel bone and rear brake create no assets"):
        for variant, suffix in [("Track", "_Track"), ("Base", "")]:
            manifest = session.call({"op": "read", "vehicle": dict(vehicle, variant=variant)})
            folder = f"Content/Vehicle/SportsCar/{name}/{name}_{variant}"
            check(set(manifest["blueprintPaths"]) == {f"{folder}/Blueprint/BP_{name}{suffix}.uasset",
                  f"{folder}/AnimBlueprint/ABP_{name}{suffix}.uasset"}, "Wrong blueprint name or folder")
        missing = copy.deepcopy(items[:-1])
        missing[0]["data"]["bones"].pop()
        answer = write(missing)
        check(not answer["ok"] and all(key in answer["error"] for key in ("D_WheelBR", "BrakeR")), str(answer))
        check(not any(classes().values()) and not list(content.rglob("*.uasset")), "Preflight left assets")

    parent_path = "/Script/UnrealSharp.WYAutomobileCS_C"
    if not session.command("INSPECT", paths=[parent_path])["classes"][parent_path]:
        with session.case("Missing WangYue parent rejects complete first creation without assets"):
            answer = write(items)
            check(not answer["ok"] and "WYAutomobileCS" in answer["error"], str(answer))
            check(not any(classes().values()), "Missing project class left partial assets")
        return

    with session.case("Collapsed tire rolls back the full vehicle; retry creates both blueprints and native undo/redo restores them"):
        collapsed = copy.deepcopy(items)
        wheel_points = collapsed[3]["data"]["positions"]
        for p in wheel_points:
            # Preserve the triangles, with area in XZ and zero width along the Y axle.
            p[1], p[2] = 0, p[1]
        answer = write(collapsed)
        check(not answer["ok"] and "Tire_0000" in answer["error"], str(answer))
        check(not any(classes().values()) and not list(content.rglob("*.uasset")), "Failed build left assets")
        answer = write(items)
        check(answer["ok"], str(answer))
        present = classes()
        check(list(present.values()).count("StaticMesh") == 4 and present[paths[0]] == "Blueprint" and
              present[paths[1]] == "AnimBlueprint" and all(present.values()), "First creation missed assets")
        session.command("UNDO")
        check(not any(classes().values()), "One undo did not remove the complete vehicle")
        session.command("REDO")
        check(classes() == present, "One redo did not restore the complete vehicle")
        driving = session.command("VEHICLE_DRIVING_DEFAULTS", blueprint=packages[0])
        check(driving["default_ai_disabled"] and driving["ai_disabled"] and driving["orbit_cameras"] == 1 and
              driving["axles"]["Front Axle"] == [60.0, True] and driving["axles"]["Rear Axle"][1],
              f"Unexpected generated vehicle driving defaults: {driving}")
        check(not list(content.rglob("*.uasset")), "First creation or redo saved the vehicle")


def check_skeleton_names(session, root):
    def vehicle(name, variant="Base"):
        return {"type": "SportsCar", "name": name, "variant": variant}

    def base(v):
        return f"/Game/Vehicle/SportsCar/{v['name']}/{v['name']}_{v['variant']}"

    def skeleton(v, suffix=""):
        return f"{base(v)}/Skeleton/SKEL_{v['name']}{suffix}"

    def object_path(package):
        return package + "." + package.rsplit("/", 1)[1]

    seed = vehicle("SkeletonSeed", "Track")
    track = skeleton(seed, "_Track")
    with session.case("New Base skeleton omits the suffix; variant skeleton retains it"):
        for v, package in [(vehicle("SkeletonSeed"), skeleton(vehicle("SkeletonSeed"))), (seed, track)]:
            session.write(v, [root], {})
            actual = session.read(v)["Root"]
            near(actual["data"], root["data"])
            check(actual.get("asset") == package, f"Outline points at {actual.get('asset')} instead of {package}")
            paths = [object_path(package)]
            if v["variant"] == "Base":
                paths.append(object_path(skeleton(v, "_Base")))
            classes = session.command("INSPECT", paths=paths)["classes"]
            check(classes[paths[0]] == "Skeleton" and not any(classes[p] for p in paths[1:]),
                  f"Unexpected skeleton objects: {classes}")

    def copy_asset(source, destination):
        session.command("COPY_TEST_SKELETON", source=object_path(source), destination=destination)

    for bind_legacy in [True, False]:
        v = vehicle("LegacyBound" if bind_legacy else "ModernBound")
        canonical, legacy = skeleton(v), skeleton(v, "_Base")
        bound, other = (legacy, canonical) if bind_legacy else (canonical, legacy)
        with session.case(f"Base skeleton read, outline and writes follow the mesh bound to {bound.rsplit('/', 1)[1]}"):
            # Seed one naming convention and let the real bridge bind a body to that specific object.
            copy_asset(track, bound)
            before = session.read(v)
            check(before["Root"].get("asset") == bound, f"Single existing skeleton was missed: {before}")
            body = mesh("LOD0_Body")
            session.write(v, [body], before)
            mesh_obj = object_path(base(v) + "/SkeletalMesh/SK_" + v["name"])
            check(session.command("MESH_SKELETON", mesh=mesh_obj)["skeleton"] == object_path(bound), "Wrong initial mesh binding")
            copy_asset(track, other)
            both = session.read(v)
            check(both["Root"].get("asset") == bound, "Outline selected the unbound skeleton")
            edit = copy.deepcopy(root)
            edit["data"]["bones"][1]["translation"] = [13, 2, -7]
            session.write(v, [edit], both)
            after = session.read(v)
            check(after["Root"].get("asset") == bound, "Editing Root changed its target path")
            near(after["Root"]["data"], edit["data"])
            check(session.command("MESH_SKELETON", mesh=mesh_obj)["skeleton"] == object_path(bound), "Editing Root rebound the mesh")
            # Inspect the otherwise hidden sibling independently by copying its current state.
            probe = vehicle(v["name"] + "Other")
            copy_asset(other, skeleton(probe))
            near(session.read(probe)["Root"]["data"], root["data"], "unbound skeleton")


def check_static_mesh_names(session):
    seed = {"type": "SportsCar", "name": "WheelParts", "variant": "Track"}
    legacy_car = dict(seed, variant="Base")

    def package(v, name):
        return f"/Game/Vehicle/{v['type']}/{v['name']}/{v['name']}_{v['variant']}/StaticMesh/{name}"

    def object_path(path):
        return path + "." + path.rsplit("/", 1)[1]

    def asset_copy(source, destination):
        session.command("COPY_TEST_STATIC", source=object_path(source), destination=destination)

    parts = ("Tire_0000", "Tire_1001", "Hub_0000", "Hub_1001", "BrakeF", "BrakeB", "BrakeR")
    lod0 = mesh("LOD0_Tire_0000", "staticMesh")
    lod1 = mesh("LOD1_Tire_0000", "staticMesh", lod=1, seed=1, triangle=True)
    canonical = package(seed, "SM_Tire_0000")
    with session.case("Wheel and brake assets use SM_ packages; protocol names and separate numbered parts survive readback"):
        items = [lod0, lod1] + [mesh("LOD0_" + part, "staticMesh", seed=i + 2) for i, part in enumerate(parts[1:])]
        session.write(seed, items, {})
        rows = session.read(seed)
        check(set(rows) == {row["id"] for row in items}, "Static asset names leaked into Max scope IDs")
        for expected in items:
            row = rows[expected["id"]]
            part = expected["name"].split("_", 1)[1]
            check(row["name"] == expected["name"] and row["asset"] == package(seed, "SM_" + part),
                  f"{expected['id']} maps to unexpected scope {row['name']} or package {row.get('asset')}")
            assert_mesh(row, expected)
        new = [object_path(package(seed, "SM_" + part)) for part in parts]
        old = [object_path(package(seed, part)) for part in parts]
        classes = session.command("INSPECT", paths=new + old)["classes"]
        check(all(classes[path] == "StaticMesh" for path in new) and not any(classes[path] for path in old),
              f"Incorrect package inventory: {classes}")

    legacy = package(legacy_car, "Tire_0000")
    current = package(legacy_car, "SM_Tire_0000")
    lod2 = mesh("LOD2_Tire_0000", "staticMesh", lod=2, triangle=True, seed=4)
    changed = copy.deepcopy(lod0)
    changed["data"]["colors"][0] = [0.123, 0.456, 0.789, 1]
    with session.case("Legacy unprefixed mesh supports editing and appending another LOD in place"):
        asset_copy(canonical, legacy)
        before = session.read(legacy_car)
        check({row["asset"] for row in before.values()} == {legacy}, "Old mesh package is not recognized")
        session.write(legacy_car, [changed, lod2], before)
        after = session.read(legacy_car)
        check(set(after) == {lod0["id"], lod1["id"], lod2["id"]} and all(v["asset"] == legacy for v in after.values()),
              "Legacy scope was split across packages")
        assert_mesh(after[lod0["id"]], changed)
        assert_mesh(after[lod1["id"]], lod1)
        assert_mesh(after[lod2["id"]], lod2)
        check(session.command("INSPECT", paths=[object_path(current)])["classes"][object_path(current)] is None,
              "Legacy update allocated a second mesh")

    with session.case("Coexisting prefixed and unprefixed assets report an ambiguity and reject writes to either"):
        asset_copy(canonical, current)
        raw = session.call({"op": "read", "vehicle": legacy_car})["items"]
        conflicts = {row["id"]: row for row in raw}
        check(len(raw) == len(conflicts) and set(conflicts) == set(after), "Ambiguous scopes are duplicated or missing")
        check(all(v.get("unmatched") and v.get("reason") for v in conflicts.values()), "Ambiguous LODs appear writable")
        hub = mesh("LOD0_Hub_0000", "staticMesh")
        extra = mesh("LOD3_Tire_0000", "staticMesh", lod=3, seed=7)
        for unsafe in (lod0, extra):
            rejected = session.write(legacy_car, [hub, unsafe], conflicts, success=False)
            check(not rejected["ok"] and "Tire_0000" in rejected["error"], str(rejected))
            check(session.read(legacy_car) == conflicts, "Rejected batch changed another asset")
        session.write(legacy_car, [hub], conflicts)
        assert_mesh(session.read(legacy_car)[hub["id"]], hub)
        for candidate, expected, variant in ((legacy, changed, "Legacy"), (current, lod0, "Prefixed")):
            probe = dict(seed, name="StaticProbe", variant=variant)
            asset_copy(candidate, package(probe, "SM_Tire_0000"))
            assert_mesh(session.read(probe)[lod0["id"]], expected)


def check_material_slots(session, root):
    name = "MaterialQA" + uuid.uuid4().hex[:8]
    car = {"type": "SportsCar", "name": name, "variant": "Base"}
    base = f"/Game/Vehicle/SportsCar/{name}/{name}_Base"
    folder = base + "/MaterialInstance"
    sk = base + "/SkeletalMesh/SK_" + name
    sm = base + "/StaticMesh/SM_Tire_0000"
    masters = {key: "/Game/Material/Vehicle/" + path for key, path in {
        "paint": "M_Vehicle_CarPaintWY_Master", "glass": "M_Vehicle_Glass_Master",
        "carbon": "M_Vehicle_CarbonFiber_Master", "pbr": "M_Vehicle_PBR_BaseMat"}.items()}
    paint, glass, carbon, rubber = "Body_carpaint_Glass", "Door_Glass_Carbon", "Kit_Carbon", "Tread"
    existing, unused = "Existing_Glass", "UnusedTrim"

    def named(source, slots, ids):
        value = copy.deepcopy(source)
        value["data"]["materialSlots"] = [{"id": slot, "name": slot_name} for slot, slot_name in slots]
        value["data"]["materialIds"] = ids
        return value

    def inspect(instances=(), meshes=None):
        result = session.command("MATERIAL_BINDINGS", instances=[f"{folder}/{n}" for n in instances],
                                 meshes=meshes or {})
        return {"instances": result["instances"], "meshes": result["meshes"]}

    def assignments(scope):
        payload = scope["data"]
        slots = {s["id"]: s for s in payload["materialSlots"]}
        return [(slots[i]["name"], slots[i].get("asset", "").split(".")[0]) for i in payload["materialIds"]]

    def check_faces(snapshot, source, names):
        target = snapshot[source["id"]]
        expected = [(n, f"{folder}/{n}") for n in names]
        check(assignments(target) == expected, f"{source['id']} face materials: {assignments(target)} != {expected}")

    body = named(mesh("LOD0_Body"), [(0, paint), (37, glass), (60, unused)], [0, 37])
    door = named(mesh("LOD0_DoorModelFL", offset=(20, 0, 0)), [(37, carbon), (0, existing)], [37, 0])
    coarse = named(mesh("LOD1_Body", lod=1), [(2, glass), (8, paint)], [2, 8])
    tire = named(mesh("LOD0_Tire_0000", "staticMesh"), [(24, rubber), (3, glass)], [24, 3])
    tire1 = named(mesh("LOD1_Tire_0000", "staticMesh", 1), [(0, glass), (1, rubber)], [0, 1])
    meshes = {sk: 2, sm: 2}
    with session.case("A missing master or incompatible existing asset aborts the material and mesh batch"):
        # An artist's existing instance has its own parent and needs no project PBR master.
        separate_car = {**car, "name": name + "Existing"}
        existing_base = base.replace(name, separate_car["name"])
        existing_only = existing_base + "/MaterialInstance/MI_" + separate_car["name"] + "_Seat"
        custom_master = "/Game/FixtureMaterials/ArtistMaster"
        session.command("MATERIAL_FIXTURE", kind="parent", package=custom_master)
        session.command("MATERIAL_FIXTURE", kind="instance", package=existing_only, parent=custom_master)
        one = named(mesh("LOD0_Tire_0000", "staticMesh"), [(0, "MI_Seat")], [0, 0])
        session.write(separate_car, [one], {})
        actual = session.read(separate_car)[one["id"]]["data"]["materialSlots"]
        check(actual == [{"id": 0, "name": "MI_Seat", "asset": existing_only}], f"Existing vehicle-qualified MI was not reused: {actual}")
        session.command("MATERIAL_FIXTURE", kind="parent", package=masters["pbr"], texture_parameters=True)
        rejected = session.write(car, [tire], {}, success=False)
        check(not rejected["ok"], "Accepted Glass without its master material")
        check(not session.read(car) and not any(inspect([rubber, glass])["instances"].values()),
              "Failed batch left a mesh or a newly created instance")
        for key in ("paint", "glass", "carbon"):
            session.command("MATERIAL_FIXTURE", kind="parent", package=masters[key])
        session.command("MATERIAL_FIXTURE", kind="conflict", package=folder + "/WrongClass")
        bad = named(mesh("LOD0_Hub_0000", "staticMesh"), [(0, "WrongClass")], [0, 0])
        rejected = session.write(car, [root, body, tire, bad], {}, success=False)
        check(not rejected["ok"] and not session.read(car), "Wrong-class object was overwritten or some meshes were created")
        check(not any(inspect([paint, rubber, glass, unused])["instances"].values()), "Failed batch created instances")
        path = f"{folder}/WrongClass.WrongClass"
        check(session.command("INSPECT", paths=[path])["classes"][path] == "MaterialFunction", "Existing asset changed class")

    with session.case("Multi-material skeletal parts and static LODs bind the correct vehicle instances and keep the artist's MI"):
        session.command("MATERIAL_FIXTURE", kind="instance", package=f"{folder}/{existing}", parent=masters["carbon"])
        session.write(car, [root, body, door, coarse, tire, tire1], {})
        before = session.read(car)
        expected_faces = [(body, [paint, glass]), (door, [carbon, existing]), (coarse, [glass, paint]),
                          (tire, [rubber, glass]), (tire1, [glass, rubber])]
        for source, names in expected_faces:
            check_faces(before, source, names)
            payload = before[source["id"]]["data"]
            check(payload["materialIds"] == source["data"]["materialIds"], f"Lost original face IDs: {payload}")
            slots = {s["id"]: s["name"] for s in payload["materialSlots"]}
            for slot in source["data"]["materialSlots"]:
                check(slots.get(slot["id"]) == slot["name"], f"Lost source slot {slot}: {slots}")
        result = inspect([paint, glass, carbon, rubber, unused, existing], meshes)
        for n, master in [(paint, "paint"), (glass, "glass"), (carbon, "carbon"), (rubber, "pbr"),
                          (unused, "pbr"), (existing, "carbon")]:
            obj = result["instances"][f"{folder}/{n}"]
            check(obj and obj["class"] == "MaterialInstanceConstant" and obj["parent"] == masters[master], f"{n}: {obj}")
        check(result["instances"][f"{folder}/{existing}"]["parameters"].get("BridgeWear") == 0.375,
              "Existing instance override was overwritten")
        for path, expected in [(sk, [{paint, glass, carbon, existing}, {paint, glass}]), (sm, [{rubber, glass}]*2)]:
            info = result["meshes"][path]
            for lod, used in enumerate(expected):
                check(set(info["sections"][lod]) == used, f"LOD {lod} render sections of {path}: {info}")
                for n in used:
                    check(info["bindings"][n] == f"{folder}/{n}", f"Mesh material slot references wrong object: {info}")
        check(result["meshes"][sk]["bindings"][unused] == f"{folder}/{unused}", "Declared unused material slot disappeared")
        session.write(car, [body, tire], before)
        before = session.read(car)
        check(inspect([paint, glass, carbon, rubber, unused, existing], meshes) == result, "Repeated push changed slots or instances")

    with session.case("A part-only material change protects sibling meshes and LODs and undoes both the new MI and binding"):
        changed = named(body, [(0, paint), (37, "NewTrim"), (60, unused)], [37, 0])
        session.write(car, [changed], before)
        after = session.read(car)
        check_faces(after, body, ["NewTrim", paint])
        for source, names in expected_faces[1:]:
            check_faces(after, source, names)
        new_path = folder + "/NewTrim"
        check(inspect(["NewTrim"])["instances"][new_path]["parent"] == masters["pbr"], "New instance missing")
        session.command("UNDO")
        check_faces(session.read(car), body, [paint, glass])
        check(inspect(["NewTrim"])["instances"][new_path] is None, "Undo left newly created material instance")
        session.command("REDO")
        check_faces(session.read(car), body, ["NewTrim", paint])
        check(inspect(["NewTrim"])["instances"][new_path]["parent"] == masters["pbr"], "Redo did not restore MI")

    with session.case("Unsaved UE slot renames and section reassignment are visible in mesh readback"):
        session.command("EDIT_MESH_SLOTS", mesh=sm, lod=0, rename=rubber, new_name="ArtistTread", target=glass)
        edited = session.read(car)
        current = ("ArtistTread", f"{folder}/{rubber}")
        check(assignments(edited[tire["id"]]) == [current, current], f"Unsaved section override lost: {assignments(edited[tire['id']])}")
        check(assignments(edited[tire1["id"]]) == [(glass, f"{folder}/{glass}"), current],
              "Renaming a shared slot must be visible in every LOD without moving other triangles")
        stale = session.write(car, [tire], before, success=False)
        check(not stale["ok"], "A stale write silently overwrote the UE artist's unsaved material edits")
        session.command("EDIT_MESH_SLOTS", mesh=sm, lod=1, new_name="Material_17", target=glass)
        fresh = session.read(car)
        read_lod = fresh[tire1["id"]]
        check(assignments(read_lod) == [("Material_17", ""), current], "UE-only slot or its face assignment was lost")
        new_id, old_id = read_lod["data"]["materialIds"]
        check(new_id not in (0, 1) and old_id == 1, "A UE-only slot reused an existing Max face ID")
        session.command("EDIT_MESH_SLOTS", mesh=sm, lod=1, new_name="None", target="Material_17")
        unnamed = session.read(car)[tire1["id"]]
        unnamed_id = unnamed["data"]["materialIds"][0]
        check(assignments(unnamed) == [(f"Material_{unnamed_id}", ""), current],
              "An unnamed UE slot could not be represented as a Max placeholder")
        session.command("EDIT_MESH_SLOTS", mesh=sk, rename=paint, new_name="ArtistBody", assign=f"{folder}/{carbon}")
        edited_skel = session.read(car)
        artist = ("ArtistBody", f"{folder}/{carbon}")
        check(assignments(edited_skel[body["id"]]) == [("NewTrim", f"{folder}/NewTrim"), artist],
              "Edited skeletal slot name or material reference was lost on LOD0")
        check(assignments(edited_skel[coarse["id"]]) == [(glass, f"{folder}/{glass}"), artist],
              "Edited skeletal slot name or material reference was lost on LOD1")
        check_faces(edited_skel, door, [carbon, existing])

        # The old wire format supplied face IDs without a named slot table.
        legacy = mesh("LOD0_Hub_1001", "staticMesh")
        legacy["data"]["materialIds"] = [0, 1]
        session.write(car, [legacy], {})
        old = session.read(car)[legacy["id"]]["data"]
        check(old["materialIds"] == [0, 1] and not old["materialSlots"],
              "Old bridge placeholders unexpectedly became named Max materials")
        legacy_mesh = base + "/StaticMesh/SM_Hub_1001"
        previous = "Material_0"
        for new_name in ("Material_17", "None"):
            session.command("EDIT_MESH_SLOTS", mesh=legacy_mesh, lod=0, new_name=new_name, target=previous)
            data = session.read(car)[legacy["id"]]["data"]
            new_id, untouched_id = data["materialIds"]
            visible = {s["id"]: (s["name"], s.get("asset")) for s in data["materialSlots"]}
            wanted = f"Material_{new_id}" if new_name == "None" else new_name
            check(untouched_id == 1 and new_id not in (0, 1, 17) and visible == {new_id: (wanted, "")},
                  f"Upgrade lost the artist's new slot or exposed an old placeholder: {data}")
            previous = new_name
        check(not list((session.directory / "Content").rglob("*.uasset")), "Material creation or mesh edits saved packages")


def check_vehicle_material_textures(session, root):
    # The car name contains a master-material keyword. Classification must use only the purpose.
    name = "Glass_QA_" + session.identifier[:6]
    car = {"type": "SportsCar", "name": name, "variant": "Track"}
    base = f"/Game/Vehicle/SportsCar/{name}/{name}_Track"
    materials, textures = base + "/MaterialInstance", base + "/Texture"
    pbr = "/Game/Material/Vehicle/M_Vehicle_PBR_BaseMat"
    default_texture = "/Engine/EngineResources/DefaultTexture"
    parameters = {"Color_Tex": "D", "M_Tex": "MRO", "Normal_Tex": "N"}

    def texture(part, suffix):
        return f"{textures}/T_{name}_{part}_{suffix}"

    def instance(part):
        return f"{materials}/MI_{name}_{part}"

    def inspect(parts):
        return session.command("MATERIAL_BINDINGS", instances=[instance(p) for p in parts], meshes={},
                               check_effective_textures=True)["instances"]

    def named_mesh(node, kind, parts):
        obj = mesh(node, kind)
        obj["data"]["materialSlots"] = [{"id": i, "name": "MI_" + p} for i, p in enumerate(parts)]
        obj["data"]["materialIds"] = [0, 1]
        return obj

    body_part, seat, authored = "Body_Trim_FL", "Seat_Pad_FL", "Custom_Trim"
    special = ["CarPaint", "Glass01", "CarbonJoin"]
    body = named_mesh("LOD0_Body", "skeletalPart", [body_part, seat, *special])
    wheel = named_mesh("LOD0_Tire_0000", "staticMesh", [body_part, authored])
    targets = [body_part, seat, authored, *special]
    all_maps = {k: texture(body_part, s) for k, s in parameters.items()}
    donor = textures + "/T_QA_Artist"
    with session.case("Vehicle-qualified MI names bind exact per-part PBR maps and preserve authored instances"):
        available = list(all_maps.values()) + [donor, texture(seat, "D"),
                                               *(texture(p, "D") for p in [authored, *special])]
        # Decoys have the expected part and suffix, but a different car or folder.
        different_variant = base.replace(name + "_Track", name + "_Base")
        available.extend([texture(seat, "MRO").replace(base, different_variant),
                          textures + "/T_AnotherCar_" + seat + "_MRO",
                          texture(seat, "N").replace("/Texture/", "/Texture/Unused/")])
        session.command("TEXTURE_FIXTURES", packages=available)
        session.command("MATERIAL_FIXTURE", kind="instance", package=instance(authored), parent=pbr, textures={"Color_Tex": donor})
        check(session.call({"op": "read", "vehicle": car})["empty"], "Textures and prebuilt MIs blocked first creation")
        session.write(car, [root, body, wheel], {})
        before = session.read(car)
        for source in (body, wheel):
            received = {s["id"]: (s["name"], s["asset"]) for s in before[source["id"]]["data"]["materialSlots"]}
            for slot in source["data"]["materialSlots"]:
                short = slot["name"]
                check(received.get(slot["id"]) == (short, instance(short[3:])), f"Wrong wire name or asset for {short}: {received}")
        short_asset = materials + "/MI_" + body_part
        short_object = short_asset + "." + short_asset.rsplit("/", 1)[1]
        check(session.command("INSPECT", paths=[short_object])["classes"][short_object] is None, "Created a short duplicate asset")
        state = inspect(targets)
        for part in (body_part, seat):
            check(state[instance(part)]["parent"] == pbr, f"Car name incorrectly selected glass master for {part}")
        check(state[instance(body_part)]["textures"] == all_maps and state[instance(body_part)]["effective"] == all_maps,
              f"Expected three local texture references: {state[instance(body_part)]}")
        seat_mi = state[instance(seat)]
        check(seat_mi["textures"] == {"Color_Tex": texture(seat, "D")} and
              seat_mi["effective"] == {"Color_Tex": texture(seat, "D"), "M_Tex": default_texture, "Normal_Tex": default_texture},
              f"Partial texture set selected wrong assets: {seat_mi}")
        missing = {k: texture(seat, suffix) for k, suffix in parameters.items() if k != "Color_Tex"}
        check(seat_mi["pending"] == {k: p + "." + p.rsplit("/", 1)[1] for k, p in missing.items()},
              f"Missing references were not retained by exact object path: {seat_mi}")
        check(state[instance(authored)]["textures"] == {"Color_Tex": donor} and state[instance(authored)]["parameters"]["BridgeWear"] == .375,
              "Bridge replaced an artist's existing overrides")
        for part, master in zip(special, ("M_Vehicle_CarPaintWY_Master", "M_Vehicle_Glass_Master", "M_Vehicle_CarbonFiber_Master")):
            mi = state[instance(part)]
            check(mi["parent"] == "/Game/Material/Vehicle/" + master and not mi["textures"] and not mi["pending"],
                  f"Non-PBR received texture configuration: {mi}")
        session.command("UNDO")
        undone = inspect(targets)
        check(all(undone[instance(p)] is None for p in targets if p != authored) and undone[instance(authored)] == state[instance(authored)],
              "Undo retained created instances or affected pre-existing instance")
        session.command("REDO")
        check(inspect(targets) == state, "Redo lost MI bindings or parameters")
        session.write(car, [body, wheel], session.read(car))
        check(inspect(targets) == state, "Repeated push modified MI textures")
        sm = base + "/StaticMesh/SM_Tire_0000"
        canonical = instance(body_part).rsplit("/", 1)[1]
        bindings = session.command("MATERIAL_BINDINGS", instances=[], meshes={sm: 1})["meshes"][sm]["bindings"]
        primary = next(slot for slot, bound in bindings.items() if bound == instance(body_part))
        session.command("EDIT_MESH_SLOTS", mesh=sm, rename=primary, new_name=canonical)
        def wheel_slot():
            slots = session.read(car)[wheel["id"]]["data"]["materialSlots"]
            return next(s["name"] for s in slots if s["id"] == 0)
        check(wheel_slot() == "MI_" + body_part, "UE resource-style slot doubled the vehicle name on pull")
        session.command("EDIT_MESH_SLOTS", mesh=sm, rename=canonical, new_name="QA_Wheel_Artist")
        check(wheel_slot() == "QA_Wheel_Artist", "Artist's distinct UE slot label did not survive")

    with session.case("Later texture imports and normal mesh syncs leave existing MI settings alone"):
        session.command("EDIT_MI_TEXTURE", instance=instance(seat), parameter="Normal_Tex", texture=donor)
        authored_state = inspect([seat])[instance(seat)]
        session.command("IMPORT_TEXTURES", packages=[texture(seat, "MRO"), texture(seat, "N")])
        # Get enough editor ticks to observe any deferred asset-registry listener.
        time.sleep(1.0)
        before = session.read(car)
        session.write(car, [body, wheel], before)
        check(inspect([seat])[instance(seat)] == authored_state, "Import or mesh sync rewrote an existing MI")
        check(not list((session.directory / "Content").rglob("*.uasset")), "Texture assignment saved packages")


def physics_packages(vehicle):
    name = vehicle["name"]
    folder = f"/Game/Vehicle/{vehicle['type']}/{name}/{name}_{vehicle['variant']}"
    return {"mesh": f"{folder}/SkeletalMesh/SK_{name}", "physics": f"{folder}/PhysicsAsset/PHYS_{name}_Body"}


def check_physics_binding(session, vehicle, hull_count):
    packages = physics_packages(vehicle)
    bindings = session.command("PHYSICS_BINDINGS", **packages)
    check(bindings["mesh_physics"] == packages["physics"] and bindings["physics_preview"] == packages["mesh"] and
          bindings["body_index"] >= 0 and bindings["body_parent"] == "Root" and bindings["hulls"] == {"Body": hull_count},
          f"Incorrect PhysicsAsset, preview mesh, or Root/Body collision binding: {bindings}")
    check(bindings["body_simulated"] and bindings["simple_as_complex"], f"Incorrect new Body physics defaults: {bindings}")


def run_suite(session):
    check_blueprint_creation(session)
    check_static_mesh_names(session)
    vehicle = {"type": "SportsCar", "name": "Regression" + session.identifier[:8], "variant": "Base"}
    static_vehicle = {**vehicle, "name": vehicle["name"] + "Static"}
    with session.case("Static-only write creates no Root, Skeleton, SkeletalMesh, or PhysicsAsset"):
        check(not session.read(static_vehicle), "Static-only fixture was not empty")
        only = mesh("LOD0_Hub_0000", "staticMesh", seed=1)
        session.write(static_vehicle, [only], {})
        actual = session.read(static_vehicle)
        check(set(actual) == {only["id"]}, f"Static write created extra scopes: {set(actual)}")
        assert_mesh(actual[only["id"]], only)
        name = static_vehicle["name"]
        base = f"/Game/Vehicle/SportsCar/{name}/{name}_Base"
        packages = [f"{base}/Skeleton/SKEL_{name}", f"{base}/Skeleton/SKEL_{name}_Base",
                    f"{base}/SkeletalMesh/SK_{name}", f"{base}/PhysicsAsset/PHYS_{name}_Body"]
        paths = [package + "." + package.rsplit("/", 1)[1] for package in packages]
        check(all(value is None for value in session.command("INSPECT", paths=paths)["classes"].values()), "Static write allocated an unrelated asset")

    root = item("skeleton", "Root", 0, {"bones": [
        {"name": "Root", "parent": "", "translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        {"name": "Body", "parent": "Root", "translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}]})
    a0 = mesh("LOD0_DoorModelFL", seed=2)
    b0 = mesh("LOD0_Body", offset=(0, 30, 0), seed=3)
    a1 = mesh("LOD1_DoorModelFL", lod=1, triangle=True, seed=4)
    b1 = mesh("LOD1_Body", lod=1, offset=(0, 30, 0), triangle=True, seed=5)
    wheel0 = mesh("LOD0_Hub_0000", "staticMesh", seed=6)
    wheel1 = mesh("LOD1_Hub_0000", "staticMesh", lod=1, triangle=True, seed=7)
    with session.case("Two skeletal parts at two distinct-topology LODs preserve every corner UV/normal/tangent/color and skin"):
        check(not session.read(vehicle), "Main fixture was not empty")
        session.write(vehicle, [root, a0, b0, a1, b1, wheel0, wheel1], {})
        baseline = session.read(vehicle)
        check(set(baseline) == {value["id"] for value in [root, a0, b0, a1, b1, wheel0, wheel1]}, "Missing/extra source scopes")
        for source in [a0, b0, a1, b1, wheel0, wheel1]:
            assert_mesh(baseline[source["id"]], source)
        near(baseline["Root"]["data"], root["data"], "Root")

    with session.case("Single-part corner edit preserves the sibling, both other LOD parts, skeleton, and static mesh"):
        edited = copy.deepcopy(a0)
        edited["data"]["uvs"][0][2] = [0.91, 0.14]
        edited["data"]["uvs"][1][2] = [0.17, 0.86]
        edited["data"]["normals"][2] = [0.6, 0, 0.8]
        edited["data"]["tangents"][2] = [0.8, 0, -0.6, -1]
        edited["data"]["colors"][2] = [0.31, 0.67, 0.49, 0.84]
        session.write(vehicle, [edited], baseline)
        before = session.read(vehicle)
        assert_mesh(before[a0["id"]], edited)
        assert_protected(baseline, before, [a0["id"]])
        stale = session.write(vehicle, [a0], baseline, success=False)
        check(not stale["ok"], "Stale revision was accepted")
        assert_protected(before, session.read(vehicle))

    with session.case("Add then delete source triangles/vertices remaps only one skeletal part"):
        added = copy.deepcopy(edited)
        data = added["data"]
        data["positions"].append([15, 5, 0])
        data["triangles"].append([1, 4, 2])
        data["weights"].append([{"bone": "Body", "weight": 1.0}])
        data["materialIds"].append(0)
        for field in ["normals", "tangents", "colors"]:
            data[field].extend(copy.deepcopy(data[field][:3]))
        for channel in data["uvs"]:
            channel.extend(copy.deepcopy(channel[:3]))
        session.write(vehicle, [added], before)
        after_add = session.read(vehicle)
        assert_mesh(after_add[a0["id"]], added)
        assert_protected(before, after_add, [a0["id"]])
        deleted = copy.deepcopy(added)
        data = deleted["data"]
        for field in ["positions", "weights"]:
            data[field] = data[field][:3]
        data["triangles"], data["materialIds"] = data["triangles"][:1], data["materialIds"][:1]
        for field in ["normals", "tangents", "colors"]:
            data[field] = data[field][:3]
        data["uvs"] = [channel[:3] for channel in data["uvs"]]
        session.write(vehicle, [deleted], after_add)
        after_delete = session.read(vehicle)
        assert_mesh(after_delete[a0["id"]], deleted)
        assert_protected(after_add, after_delete, [a0["id"]])

    with session.case("LOD1 edits preserve LOD0 and the other LOD1 part; static LOD edits remain independent"):
        edit_lod = copy.deepcopy(a1)
        edit_lod["data"]["positions"][1][2] = 2.75
        edit_lod["data"]["uvs"][1][1] = [0.83, 0.21]
        session.write(vehicle, [edit_lod], after_delete)
        after_lod = session.read(vehicle)
        assert_mesh(after_lod[a1["id"]], edit_lod)
        assert_protected(after_delete, after_lod, [a1["id"]])
        edit_static = copy.deepcopy(wheel1)
        edit_static["data"]["positions"][2][2] = -3.125
        session.write(vehicle, [edit_static], after_lod)
        after_static = session.read(vehicle)
        assert_mesh(after_static[wheel1["id"]], edit_static)
        assert_protected(after_lod, after_static, [wheel1["id"]])

    with session.case("Mixed invalid batch fails before either scope changes"):
        valid = copy.deepcopy(deleted)
        valid["data"]["positions"][0][0] = -1.5
        invalid = copy.deepcopy(wheel0)
        invalid["data"]["triangles"][0][0] = 99999
        result = session.write(vehicle, [valid, invalid], after_static, success=False)
        check(not result["ok"], "Invalid mesh batch was accepted")
        assert_protected(after_static, session.read(vehicle))

    with session.case("One native editor undo/redo restores a two-scope batch"):
        first, second = copy.deepcopy(edit_lod), copy.deepcopy(wheel0)
        first["data"]["colors"][0] = [0.9, 0.1, 0.2, 0.3]
        second["data"]["uvs"][0][1] = [0.79, 0.87]
        session.write(vehicle, [first, second], after_static)
        after_batch = session.read(vehicle)
        assert_mesh(after_batch[first["id"]], first)
        assert_mesh(after_batch[second["id"]], second)
        assert_protected(after_static, after_batch, [first["id"], second["id"]])
        session.command("UNDO")
        assert_protected(after_static, session.read(vehicle))
        session.command("REDO")
        assert_protected(after_batch, session.read(vehicle))

    with session.case("Build-only collision returns closed enclosing hulls without changing any UE asset"):
        body_shape = concave_prism()
        spoiler_shape = concave_prism("Spoiler", 0, 2, (30, 0, 12))
        body_input = item("hullInput", "LOD0_Body", 0, body_shape)
        spoiler_input = item("hullInput", "LOD0_Spoiler", 0, spoiler_shape)
        built = session.call({"op": "hulls", "settings": {"mode": "single", "plane": "auto", "splitZ": 0},
                              "items": [body_input, spoiler_input]})
        shapes = {shape["name"]: shape for shape in built["items"][0]["data"]["shapes"]}
        check(set(shapes) == {"Body", "Spoiler"}, f"Unexpected hull outputs: {set(shapes)}")
        verify_convex(shapes["Body"], body_shape["positions"])
        verify_convex(shapes["Spoiler"], spoiler_shape["positions"])
        assert_protected(after_batch, session.read(vehicle))
        check("BodyCollider" not in session.read(vehicle), "Building hulls wrote PhysicsAsset")
        check(not list((session.directory / "Content").rglob("*.uasset")), "An operation saved assets without request")

    with session.case("Each advanced hull control simplifies whole, split, and spoiler hulls independently"):
        defaults = {"maxHullVertices": -1, "degenerateEdgeTolerance": 0, "skipAtHullDistanceAbsolute": 0,
                    "skipAtHullDistanceAsFraction": 2e-7}
        for mode, names in [({"mode": "single"}, {"Body", "Spoiler"}),
                            ({"mode": "split", "plane": "manual", "splitZ": 3}, {"BodyLower", "BodyUpper", "Spoiler"})]:
            def generate(**overrides):
                reply = session.call({"op": "hulls", "settings": {**mode, **overrides}, "items": [body_input, spoiler_input]})
                check(reply.get("hullSimplification") is True, "Missing hull simplification capability receipt")
                shapes = {s["name"]: s for s in reply["items"][0]["data"]["shapes"]}
                check(set(shapes) == names, f"Unexpected hulls for {mode}, {overrides}")
                return shapes

            legacy = generate()
            check(all(len(s["positions"]) > 4 for s in legacy.values()), "Fixture must exercise simplification")
            check(generate(**defaults) == legacy, "Explicit Max defaults changed the legacy hull")
            for overrides in [{"maxHullVertices": 4}, {"degenerateEdgeTolerance": 100},
                              {"skipAtHullDistanceAbsolute": 100}, {"skipAtHullDistanceAsFraction": 2},
                              {"degenerateEdgeTolerance": sys.float_info.max},
                              {"skipAtHullDistanceAbsolute": sys.float_info.max},
                              {"skipAtHullDistanceAsFraction": sys.float_info.max}]:
                for name, shape in generate(**overrides).items():
                    check(len(shape["positions"]) == 4, f"{name} did not respond to {overrides}")
                    verify_convex(shape, shape["positions"])

            for budget in [-2147483648, 0, 2147483647]:
                unlimited = generate(maxHullVertices=budget, skipAtHullDistanceAsFraction=0)
                for name in names:
                    # A positive budget can change triangulation even when it never reaches the cap.
                    # The fixture has flat sides, which may retain different collinear points.
                    verify_convex(unlimited[name], legacy[name]["positions"])
                    verify_convex(legacy[name], unlimited[name]["positions"])
        assert_protected(after_batch, session.read(vehicle))

    with session.case("Advanced hull settings reject invalid numbers before returning any geometry"):
        invalid = {"maxHullVertices": [-2147483649, 1, 2, 3, 4.5, 2147483648, "4", True, None],
                   "degenerateEdgeTolerance": [-1, "0.1", True, None],
                   "skipAtHullDistanceAbsolute": [-1, "0", True, None],
                   "skipAtHullDistanceAsFraction": [-0.1, "0.001", False, None]}
        for field, values in invalid.items():
            for value in values:
                rejected = session.call({"op": "hulls", "settings": {"mode": "single", field: value},
                                         "items": [body_input]}, success=False)
                check(rejected.get("ok") is False and "items" not in rejected and rejected.get("error"),
                      f"Invalid hull setting accepted: {field}={value!r}")
                check(field not in rejected["error"], f"Protocol field name leaked into the UI error: {rejected['error']}")
        assert_protected(after_batch, session.read(vehicle))

    with session.case("Whole and split collision include door windows and door panels and exclude the Mirror family"):
        settings = {"mode": "split", "plane": "manual", "splitZ": 3}
        door_shape = concave_prism(offset=(-20, 0, 0))
        hull_inputs = [body_input, spoiler_input, item("hullInput", "LOD0_DoorModelFL", 0, door_shape)]
        window_shape = concave_prism(z0=-30, z1=60, offset=(1000, -700, 0))
        windows = [item("skeletalPart", "LOD0_DoorWindow" + side + suffix, 0, window_shape)
                   for side in ("FL", "FR", "BL", "BR", "RL", "RR") for suffix in ("", "_0007")]
        mirrors = [item("hullInput", "LOD0_Mirror" + side + suffix, 0, window_shape)
                   for side in ("FL", "FR", "Center") for suffix in ("", "_0000")]
        def hulls(options, inputs):
            response = session.call({"op": "hulls", "settings": options, "items": inputs})
            check("worldZ" not in response, "A request without explicit worldZ received a projection receipt")
            return {s["name"]: {tuple(p) for p in s["positions"]} for s in response["items"][0]["data"]["shapes"]}

        for options in [{"mode": "single"}, settings]:
            reference = hulls(options, hull_inputs)
            check(reference == hulls(options, hull_inputs + mirrors), f"Outlying mirrors enlarged collision for {options}")
            body_hulls = ["Body"] if options["mode"] == "single" else ["BodyUpper", "BodyLower"]
            for window in windows:
                with_window = hulls(options, hull_inputs + [window])
                check(all(max(x for x, _, _ in with_window[name]) >= 1000 for name in body_hulls),
                      f"{window['name']} did not contribute to the body hulls for {options}")
                check(with_window["Spoiler"] == reference["Spoiler"], "A door window changed the separate spoiler hull")
            if options["mode"] == "split":
                check(set(reference) == {"BodyUpper", "BodyLower", "Spoiler"}, "Missing expected split hulls")
                upper, lower = reference["BodyUpper"], reference["BodyLower"]
                check(min(z for _, _, z in upper) == max(z for _, _, z in lower) == settings["splitZ"],
                      "Bounds did not meet at the exact manual cut height")
                check(all(min(x for x, _, _ in hull) == -20 for hull in (upper, lower)),
                      "Door panel is missing from a body half")
            else:
                check(set(reference) == {"Body", "Spoiler"} and min(x for x, _, _ in reference["Body"]) == -20,
                      "Whole-body hull lost the door panel or spoiler")
        assert_protected(after_batch, session.read(vehicle))

    with session.case("Split collision rejects automatic, missing, nonnumeric, and out-of-bounds cut heights"):
        invalid_settings = [{"mode": "split"}, {"mode": "split", "splitZ": 3}, {"mode": "split", "plane": "auto"},
                            {"mode": "split", "plane": "auto", "splitZ": 3}, {"mode": "split", "plane": "manual"}]
        invalid_settings.extend({"mode": "split", "plane": "manual", "splitZ": z} for z in (None, "3", True, 0, 8, 10))
        for options in invalid_settings:
            response = session.call({"op": "hulls", "settings": options, "items": [body_input]}, success=False)
            check(response.get("ok") is False and response.get("error") and "items" not in response,
                  f"Invalid cut unexpectedly produced hulls for {options}: {response}")
        assert_protected(after_batch, session.read(vehicle))

    with session.case("Tilted and translated vehicle frames split on the preview's world Z plane in local mesh coordinates"):
        tetra = {"positions": [[0, 0, 0], [10, 0, 0], [0, 10, 0], [0, 0, 10]],
                 "triangles": [[0, 2, 1], [0, 1, 3], [0, 3, 2], [1, 2, 3]]}
        source = item("hullInput", "LOD0_Body", 0, tetra)
        options = {"mode": "split", "plane": "manual", "splitZ": 115, "worldZ": [0, 0.6, 0.8, 110]}
        response = session.call({"op": "hulls", "settings": options, "items": [source]})
        check(response["splitZ"] == 115, "Reply did not report the requested scene height")
        check(response.get("worldZ") == options["worldZ"], "Split hull reply is missing the exact projection receipt")
        whole = session.call({"op": "hulls", "settings": {"mode": "single", "worldZ": options["worldZ"]},
                              "items": [source]})
        check(whole.get("worldZ") == options["worldZ"], "Whole hull reply is missing the explicit projection receipt")
        hulls = {s["name"]: s for s in response["items"][0]["data"]["shapes"]}
        check(set(hulls) == {"BodyLower", "BodyUpper"}, "Expected two body halves")

        def height(point):
            return 0.6 * point[1] + 0.8 * point[2] + 110

        y_cut, z_cut = 5 / 0.6, 5 / 0.8
        corners = [[0, y_cut, 0], [10 - y_cut, y_cut, 0], [0, 0, z_cut], [10 - z_cut, 0, z_cut]]
        for name, shape in hulls.items():
            sign = 1 if name == "BodyUpper" else -1
            check(all(sign * (height(point) - 115) >= -1.e-5 for point in shape["positions"]),
                  f"{name} crosses the preview plane")
            seam = [p for p in shape["positions"] if abs(height(p) - 115) < 1.e-5]
            check(len(seam) == len(corners) and all(any(math.dist(p, q) < 1.e-5 for p in seam) for q in corners),
                  f"{name} does not meet the expected local-space preview intersection: {seam}")
            verify_convex(shape, [p for p in tetra["positions"] if sign * (height(p) - 115) > 0] + corners)

        for world_z in (None, [], [0, 0, 1], [0, 0, 0, 110], ["0", 0, 1, 110], [1, 0, 0, None],
                        [False, 0, 1, 110], [1.e308, 0, 0, 0]):
            rejected = session.call({"op": "hulls", "settings": dict(options, worldZ=world_z), "items": [source]},
                                    success=False)
            check(rejected.get("ok") is False and "items" not in rejected, f"Invalid worldZ accepted: {world_z}")
        rejected = session.call({"op": "hulls", "settings": dict(options, splitZ=5), "items": [source]}, success=False)
        check(rejected.get("ok") is False, "A cut inside the local bounds but below the world bounds was accepted")

    with session.case("Door windows and mirrors both synchronize in the skeletal mesh"):
        target = dict(vehicle, name="DoorWindowSync")
        parts = [mesh("LOD0_DoorWindowFL", seed=8), mesh("LOD0_DoorWindowFR_0007", seed=9),
                 mesh("LOD0_MirrorFL_0000", seed=10), mesh("LOD0_MirrorFR", seed=11)]
        session.write(target, [root, *parts], {})
        synced = session.read(target)
        check(set(synced) == {"Root", *(part["id"] for part in parts)}, "Unexpected skeletal part inventory")
        for part in parts:
            assert_mesh(synced[part["id"]], part)

    with session.case("Concave manual collision is converted on explicit sync; protected assets and native undo survive"):
        physics = item("physics", "BodyCollider", 0, {"shapes": [body_shape]})
        session.write(vehicle, [physics], after_batch)
        after_physics = session.read(vehicle)
        check(set(after_physics) == set(after_batch) | {"BodyCollider"}, "Collision write changed scope inventory")
        assert_protected(after_batch, {key: value for key, value in after_physics.items() if key != "BodyCollider"})
        written = after_physics["BodyCollider"]["data"]["shapes"]
        check_physics_binding(session, vehicle, 1)
        mesh_path = physics_packages(vehicle)["mesh"]
        # The fixture prism spans (0, 0, 0) to (20, 20, 8) cm, with Body in its reference pose.
        hull_bounds = [[10, 10, 4], [10, 10, 4]]
        near(session.command("SKELETAL_COMPONENT_BOUNDS", mesh=mesh_path)["bounds"], hull_bounds, "synced physics bounds")
        check(len(written) == 1 and written[0]["name"] == body_shape["name"], "Collision group name/count changed")
        verify_convex(written[0], body_shape["positions"])
        check(len(written[0]["positions"]) < len(body_shape["positions"]), "Concave vertices were not converted to a convex hull")
        session.command("UNDO")
        assert_protected(after_batch, session.read(vehicle))
        undone = session.command("SKELETAL_COMPONENT_BOUNDS", mesh=mesh_path)
        near(undone["bounds"], undone["mesh_bounds"], "bounds after undoing collision creation")
        session.command("REDO")
        assert_protected(after_physics, session.read(vehicle))
        check_physics_binding(session, vehicle, 1)
        near(session.command("SKELETAL_COMPONENT_BOUNDS", mesh=mesh_path)["bounds"], hull_bounds, "redone physics bounds")
        check(not list((session.directory / "Content").rglob("*.uasset")), "Bridge unexpectedly saved packages")

    with session.case("Closed editable convex faces survive Chaos cooking and live UE edits still change the revision"):
        target = dict(vehicle, name="PhysicsQACoplanar")
        # The fifth point lies inside the tetrahedron's base face and is shared by three triangles.
        shape = {"name": "Body", "positions": [[0, 0, 0], [12, 0, 0], [0, 12, 0], [0, 0, 12], [4, 4, 0]],
                 "triangles": [[0, 2, 4], [2, 1, 4], [1, 0, 4], [0, 1, 3], [1, 2, 3], [2, 0, 3]]}
        verify_convex(shape, shape["positions"])
        collision = item("physics", "BodyCollider", 0, {"shapes": [shape]})
        session.write(target, [root, collision], {})
        before = session.read(target)
        near(before["BodyCollider"]["data"], collision["data"], "BodyCollider source geometry")
        session.command("MOVE_TEST_CONVEX", physics=physics_packages(target)["physics"])
        after = session.read(target)
        moved = dict(shape, positions=[[x, y, z + 3] for x, y, z in shape["positions"]])
        near(after["BodyCollider"]["data"], {"shapes": [moved]}, "Edited BodyCollider geometry")
        check(after["BodyCollider"]["revision"] != before["BodyCollider"]["revision"], "Live convex edit was missed")
        session.write(target, [collision], before, success=False)
        assert_protected(after, session.read(target))

    with session.case("Collision created before the first skinned mesh links to it when it arrives"):
        target = dict(vehicle, name="PhysicsQA" + session.identifier[:8])
        session.write(target, [root, physics], {})
        previous = session.read(target)
        check(set(previous) == {"Root", "BodyCollider"}, "Physics-only write created an unwanted mesh")
        session.write(target, [b0], previous)
        check_physics_binding(session, target, 1)

    with session.case("Collision and skinned-part writes repair a legacy cube preview and preserve explicit artist references"):
        packages = physics_packages(target)
        cube = "/Engine/EngineMeshes/SkeletalCube"

        def current_links():
            current = session.command("PHYSICS_BINDINGS", **packages)
            return current["mesh_physics"], current["physics_preview"]

        for scope in (physics, b0):
            session.command("SET_PHYSICS_BINDINGS", **packages, mesh_physics=None, physics_preview=cube)
            check(current_links() == (None, cube), "Could not create the legacy cube fixture")
            legacy = session.read(target)
            check("BodyCollider" in legacy, "A stale preview hid the existing vehicle BodyCollider")
            check(current_links() == (None, cube), "A read modified the legacy references")
            session.write(target, [scope], legacy)
            check_physics_binding(session, target, 1)
            session.command("UNDO")
            restored = current_links()
            check(restored == (None, cube), f"Undo after {scope['id']} did not restore the legacy references: {restored}")
            session.command("REDO")
            check_physics_binding(session, target, 1)
        authored = physics_packages(vehicle)
        session.command("SET_PHYSICS_BINDINGS", **packages, mesh_physics=authored["physics"], physics_preview=authored["mesh"])
        session.write(target, [physics], session.read(target))
        check(current_links() == (authored["physics"], authored["mesh"]), "Explicit artist references were overwritten")

    with session.case("A Body bone outside Root/Body cannot receive vehicle collision"):
        target = dict(vehicle, name="WrongBodyHierarchy")
        nested = copy.deepcopy(root)
        bones = nested["data"]["bones"]
        bones.insert(1, dict(bones[1], name="Auxiliary"))
        bones[-1]["parent"] = "Auxiliary"
        session.write(target, [nested, physics], {}, success=False)
        check(not session.read(target), "Rejected collision created an unrelated skeleton or physics package")

    with session.case("First create rejects planar collision atomically and accepts generated split hulls with a small bevel"):
        target = dict(vehicle, name="SplitBodyFirstCreate")
        name = target["name"]
        base = f"/Game/Vehicle/{target['type']}/{name}/{name}_Base"
        packages = [f"{base}/Skeleton/SKEL_{name}", f"{base}/SkeletalMesh/SK_{name}",
                    f"{base}/StaticMesh/SM_Hub_0000", f"{base}/PhysicsAsset/PHYS_{name}_Body"]
        paths = [package + "." + package.rsplit("/", 1)[1] for package in packages]
        planar = {"name": "BodyLower", "positions": [[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0]],
                  "triangles": [[0, 1, 2], [0, 2, 3]]}
        invalid = item("physics", "BodyCollider", 0, {"shapes": [planar]})
        rejected = session.write(target, [root, b0, wheel0, invalid], {}, success=False)
        check(not rejected.get("ok") and "BodyLower" in rejected.get("error", ""),
              f"Planar hull was not rejected by name: {rejected}")
        check(not session.read(target), "Rejected first create left asset scopes in the editor")
        inspected = session.command("INSPECT", paths=packages + paths)["classes"]
        check(all(value is None for value in inspected.values()), f"Rejected first create left live packages or assets: {inspected}")
        check(not list((session.directory / "Content").rglob("*.uasset")), "Rejected first create saved packages")

        source = item("hullInput", "LOD0_Body", 0, corner_clipped_cube())
        built = session.call({"op": "hulls", "settings": {"mode": "split", "plane": "manual", "splitZ": 5},
                              "items": [source]})["items"][0]
        expected = {shape["name"]: shape for shape in built["data"]["shapes"]}
        check(set(expected) == {"BodyLower", "BodyUpper"}, "Split did not return both body hulls")
        for shape in expected.values():
            # 3ds Max stores TriObject coordinates as Point3 (binary32).
            shape["positions"] = [list(struct.unpack("3f", struct.pack("3f", *p))) for p in shape["positions"]]
        session.write(target, [root, b0, wheel0, built], {})
        created = session.read(target)
        check(set(created) == {"Root", b0["id"], wheel0["id"], "BodyCollider"}, "Retry missed first-create assets")
        actual = {shape["name"]: shape for shape in created["BodyCollider"]["data"]["shapes"]}
        check_physics_binding(session, target, len(expected))
        check(set(actual) == set(expected), "PhysicsAsset lost a split after first create")
        for key, shape in actual.items():
            verify_convex(shape, expected[key]["positions"])
        check(not list((session.directory / "Content").rglob("*.uasset")), "Successful retry saved packages")

    with session.case("First creation detects a newly occupied vehicle folder before any writes"):
        rear = mesh("LOD0_BrakeR", "staticMesh")
        before = session.read(static_vehicle)
        rejected = session.call({"op": "write", "vehicle": static_vehicle, "items": [rear],
                                 "expected": {rear["id"]: ""}, "createVehicleBlueprints": True}, success=False)
        check(not rejected.get("ok") and "已有" in rejected.get("error", ""), "First create failed to protect existing assets")
        check(session.read(static_vehicle) == before, "Occupied folder was modified")

    check_skeleton_names(session, root)
    check_material_slots(session, root)
    check_vehicle_material_textures(session, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "build_and_deploy.ini")
    parser.add_argument("--startup-timeout", type=float, default=360, help="Seconds allowed for this isolated UE editor to initialize")
    args = parser.parse_args()
    session = EditorSession(args.config.resolve(), args.startup_timeout)
    error = None
    try:
        check(os.name == "nt", "This test requires Windows and the configured Win64 UE editor")
        session.start()
        run_suite(session)
    except (Exception, KeyboardInterrupt):
        error = traceback.format_exc()
        print(error, file=sys.stderr, flush=True)
    finally:
        try:
            session.close()
        except Exception:
            error = (error or "") + "\nCleanup failed:\n" + traceback.format_exc()
        if session.directory.exists():
            report = {"passed": error is None, "error": error, "seconds": round(time.monotonic() - session.started, 3),
                      "environment": session.info, "cases": session.results}
            (session.directory / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
            print(f"Report: {session.directory / 'report.json'}", flush=True)
    if not error:
        print(f"PASS {len(session.results)} real UE integration cases; only the owned editor was closed.", flush=True)
    return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())

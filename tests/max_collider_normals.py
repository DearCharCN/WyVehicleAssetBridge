"""Check convex meshes after Build BodyCollider or UE -> Max, inside 3ds Max.

Run the file from Max's Python scripting environment with BodyCollider in the
current scene, or call assert_outward(inspect_body_collider(parent)) in a test.
The probe only reads the evaluated mesh. It also accepts an individual mesh via
inspect_shape, so a host can check a known-good box under the same transform.
"""

import json
import math

from pymxs import runtime as rt


def inspect_shape(node):
    # Query normals and points on the same TriMesh value. This keeps both in
    # object space, including when the Max node has a reflected parent.
    mesh = node.mesh
    if mesh.numverts < 4 or mesh.numfaces < 4:
        raise AssertionError(f"{node.name} has no 3D convex surface")
    points = [tuple(rt.getVert(mesh, i)) for i in range(1, mesh.numverts + 1)]
    center = [math.fsum(p[k] for p in points) / len(points) for k in range(3)]
    distances = []
    for i in range(1, mesh.numfaces + 1):
        face = [points[int(index) - 1] for index in rt.getFace(mesh, i)]
        normal = tuple(rt.getFaceNormal(mesh, i))
        norm = math.hypot(*normal)
        distance = 0.0
        if norm > 0:
            delta = [math.fsum(p[k] - center[k] for p in face) / 3 for k in range(3)]
            distance = math.fsum(n * d for n, d in zip(normal, delta)) / norm
        if not math.isfinite(distance):
            raise AssertionError(f"{node.name}, face {i} has a non-finite normal or position")
        distances.append(distance)
    return {
        "faces": mesh.numfaces,
        "outward": sum(d > 0 for d in distances),
        "inward": sum(d < 0 for d in distances),
        "degenerate": distances.count(0),
        "minDistanceObjectUnits": min(distances),
    }


def inspect_body_collider(parent):
    if parent is None:
        raise AssertionError("Missing BodyCollider in this scene")
    shapes = {}
    for node in parent.children:
        if node.name in shapes:
            raise AssertionError(f"Duplicate convex shape name: {node.name}")
        shapes[node.name] = inspect_shape(node)
    if not shapes:
        raise AssertionError("BodyCollider has no convex meshes")
    return shapes


def assert_outward(shapes):
    for name, report in shapes.items():
        if report["outward"] != report["faces"]:
            raise AssertionError(f"{name} has incorrectly oriented convex faces: {report}")


if __name__ == "__main__":
    parents = [node for node in rt.objects if node.name in ("BodyCollider", "BodyColider")]
    if len(parents) != 1:
        raise AssertionError(f"Expected one BodyCollider root (including legacy spelling), found {len(parents)}")
    result = inspect_body_collider(parents[0])
    print(json.dumps(result, indent=2))
    assert_outward(result)

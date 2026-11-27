#include "MaxColliderPreview.h"
#include "MaxScene.h"
#include <max.h>
#include <geom/dpoint3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <optional>

namespace
{
// A plane intersects at most six edges of a frustum (or coincides with a face).
struct PlaneSection
{
    std::array<Point3, 12> points;
    int count = 0;
    void Add(const DPoint3 &p, float z)
    {
        const Point3 vertex(static_cast<float>(p.x), static_cast<float>(p.y), z);
        if (std::isfinite(vertex.x) && std::isfinite(vertex.y) &&
            std::find(points.begin(), points.begin() + count, vertex) == points.begin() + count)
            points[count++] = vertex;
    }
};

PlaneSection CutViewport(ViewExp &view, float z)
{
    PlaneSection section;
    GraphicsWindow *gw = view.getGW();
    const int w = gw->getWinSizeX(), h = gw->getWinSizeY();
    if (w <= 0 || h <= 0)
        return section;
    float projection[4][4]{}, nearDepth = 0, farDepth = 0;
    Matrix3 cameraTM;
    int perspective = 0;
    gw->getCameraMatrix(projection, &cameraTM, &perspective, &nearDepth, &farDepth);
    if (!std::isfinite(nearDepth) || !std::isfinite(farDepth) || farDepth <= nearDepth)
        return section;

    Matrix3 worldToView;
    view.GetAffineTM(worldToView);
    const Matrix3 toWorld = Inverse(worldToView);
    const DPoint3 eye(toWorld.GetTrans()), right(toWorld.GetRow(0)), up(toWorld.GetRow(1)), back(toWorld.GetRow(2));
    const IPoint2 pixels[] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    DPoint3 nearCorners[4], farCorners[4];
    for (int i = 0; i < 4; ++i)
    {
        // At one unit of forward depth, a screen position determines a ray
        // slope in perspective and a lateral displacement in orthographic views.
        const Point3 p = view.MapScreenToView(pixels[i], -1.f);
        const DPoint3 lateral = right * double(p.x) + up * double(p.y);
        const DPoint3 origin = perspective ? eye : eye + lateral;
        const DPoint3 step = perspective ? lateral - back : -back;
        nearCorners[i] = origin + step * double(nearDepth);
        farCorners[i] = origin + step * double(farDepth);
        if (step.z != 0)
        {
            const double depth = (z - origin.z) / step.z;
            if (depth >= nearDepth && depth <= farDepth)
                section.Add(origin + step * depth, z);
        }
    }
    // The slice can also enter via a near/far rim, especially at the horizon.
    for (const auto *rim : {nearCorners, farCorners})
        for (int i = 0; i < 4; ++i)
        {
            const DPoint3 &a = rim[i], &b = rim[(i + 1) % 4];
            if (a.z == z)
                section.Add(a, z);
            else if ((a.z < z && b.z > z) || (a.z > z && b.z < z))
                section.Add(a + (b - a) * ((z - a.z) / (b.z - a.z)), z);
        }
    if (section.count >= 3)
    {
        DPoint3 center(0, 0, 0);
        for (int i = 0; i < section.count; ++i)
            center += DPoint3(section.points[i]);
        center /= double(section.count);
        std::sort(section.points.begin(), section.points.begin() + section.count, [&center](const Point3 &a, const Point3 &b) {
            return std::atan2(a.y - center.y, a.x - center.x) < std::atan2(b.y - center.y, b.x - center.x);
        });
    }
    return section;
}

class ColliderPlane final : public ViewportDisplayCallback
{
public:
    void Show(double height)
    {
        if (!std::isfinite(height))
        {
            Hide();
            return;
        }
        heightCm = height;
        auto *core = GetCOREInterface();
        if (!registered || sampledTime != core->GetTime())
            UpdateUnits(core->GetTime());
        if (!registered)
        {
            registered = true;
            core->RegisterViewportDisplayCallback(FALSE, this);
        }
        core->RedrawViews(core->GetTime(), REDRAW_NORMAL);
    }

    void Hide()
    {
        if (!registered)
            return;
        registered = false;
        auto *core = GetCOREInterface();
        core->UnRegisterViewportDisplayCallback(FALSE, this);
        centimetersPerUnit.reset();
        core->ForceCompleteRedraw();
    }

    BOOL Foreground() override { return TRUE; }

    void GetViewportRect(TimeValue, ViewExp *view, Rect *rect) override
    {
        if (view && rect && view->IsAlive() && view->getGW())
        {
            GraphicsWindow *gw = view->getGW();
            *rect = Rect(IPoint2(0, 0), IPoint2(gw->getWinSizeX(), gw->getWinSizeY()));
        }
    }

    void Display(TimeValue time, ViewExp *view, int) override
    {
        if (!registered || !view || !view->IsAlive() || !view->getGW())
            return;
        if (time != sampledTime)
            UpdateUnits(time);
        if (!centimetersPerUnit)
            return;
        const float z = static_cast<float>(heightCm / *centimetersPerUnit);
        if (!std::isfinite(z))
            return;
        auto *gw = view->getGW();
        auto section = CutViewport(*view, z);
        if (section.count < 2)
            return;
        const Matrix3 previousTransform = gw->getTransform();
        const DWORD previousLimits = gw->getRndLimits();
        constexpr DWORD transparentPass = GW_TRANSPARENCY | GW_TRANSPARENT_PASS;
        const DWORD previousTransparency = gw->getRndMode() & transparentPass;
        const Material *currentMaterial = gw->getMaterial();
        const Material previousMaterial = currentMaterial ? *currentMaterial : Material();

        const Point3 white(1.f, 1.f, 1.f);
        Material material;
        material.Ka = material.Kd = white;
        material.Ks = Point3(0.f, 0.f, 0.f);
        material.selfIllum = 1.f;
        material.opacity = 0.85f;
        material.dblSided = TRUE;
        gw->setTransform(Matrix3());
        gw->setRndLimits(GW_COLOR_VERTS | GW_Z_BUFFER | transparentPass);
        gw->setMaterial(material);
        gw->setTransparency(transparentPass);
        std::array<Point3, 13> colors;
        colors.fill(white);
        gw->startTriangles();
        for (int i = 2; i < section.count; ++i)
        {
            Point3 triangle[] = {section.points[0], section.points[i - 1], section.points[i]};
            gw->triangle(triangle, colors.data());
        }
        gw->endTriangles();
        // The perimeter also shows the cut in a perfectly edge-on front or side view.
        // GraphicsWindow requires a spare vertex after the last point of a closed polyline.
        std::array<Point3, 13> outline;
        std::copy_n(section.points.begin(), section.count, outline.begin());
        outline[section.count] = outline[0];
        gw->polyline(section.count, outline.data(), colors.data(), section.count > 2, nullptr);
        gw->setRndLimits(previousLimits);
        gw->setMaterial(previousMaterial);
        gw->setTransparency(previousTransparency);
        gw->setTransform(previousTransform);
    }

private:
    void UpdateUnits(TimeValue time)
    {
        sampledTime = time;
        centimetersPerUnit.reset();
        try
        {
            centimetersPerUnit = WyColliderPreviewUnitScale(time);
        }
        catch (const std::exception &)
        {
            // The build command reports ambiguous scene nodes. A viewport callback
            // must not propagate a scene-validation error into Max's display loop.
        }
    }

    double heightCm = 0;
    bool registered = false;
    TimeValue sampledTime = 0;
    std::optional<double> centimetersPerUnit;
};
ColliderPlane preview;
}

void WyShowColliderPlane(double heightCm)
{
    preview.Show(heightCm);
}

void WyHideColliderPlane()
{
    preview.Hide();
}

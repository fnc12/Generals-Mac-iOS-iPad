using System.Numerics;
using EndEditor.Core;

namespace EndEditor.Render;

/// The map view's camera, as MapViewState on macOS: an orbit around a ground target, z up.
/// View coordinates are pixels of the view with the origin top left.
public sealed class MapCamera
{
    public Vector3 Target;
    public float Yaw = -MathF.PI / 2;  // camera south of the target, looking north
    public float Pitch = 0.85f;
    public float Distance = 1000;
    public const float FovY = 0.8f;

    public Vector3 Eye => Target + Distance * new Vector3(MathF.Cos(Pitch) * MathF.Cos(Yaw),
                                                          MathF.Cos(Pitch) * MathF.Sin(Yaw), MathF.Sin(Pitch));

    /// Row-vector convention (v * M), right-handed, depth 0..1.
    public Matrix4x4 ViewProjection(float aspect)
    {
        var view = Matrix4x4.CreateLookAt(Eye, Target, Vector3.UnitZ);
        var projection = Matrix4x4.CreatePerspectiveFieldOfView(FovY, aspect, MathF.Max(Distance * 0.02f, 1),
                                                                Distance * 6 + 5000);
        return view * projection;
    }

    /// Frames the playable area from the south.
    public void Frame(MapScene scene, bool top = false)
    {
        var rect = scene.Playable;
        if (rect.Z <= rect.X || rect.W <= rect.Y)
        {
            rect = new Vector4(0, 0, Math.Max(scene.Width - 1 - 2 * scene.Border, 1) * 10f,
                               Math.Max(scene.Height - 1 - 2 * scene.Border, 1) * 10f);
        }
        var center = new Vector2((rect.X + rect.Z) / 2, (rect.Y + rect.W) / 2);
        Target = new Vector3(center.X, center.Y, scene.GroundHeight(center.X, center.Y));
        Yaw = -MathF.PI / 2;
        Pitch = top ? 1.5f : 0.85f;
        var size = MathF.Max(rect.Z - rect.X, rect.W - rect.Y);
        Distance = size * 0.5f / MathF.Tan(FovY / 2) * (top ? 1.05f : 1.15f);
    }

    /// A drag in pixels: the ground under the cursor follows it.
    public void Pan(float dx, float dy, float viewHeight)
    {
        var scale = 2 * Distance * MathF.Tan(FovY / 2) / MathF.Max(viewHeight, 1);
        var forward = new Vector3(-MathF.Cos(Yaw), -MathF.Sin(Yaw), 0);
        var right = new Vector3(forward.Y, -forward.X, 0);
        Target -= right * dx * scale;
        Target += forward * dy * scale / MathF.Max(MathF.Sin(Pitch), 0.35f);
    }

    public void Rotate(float dx, float dy)
    {
        Yaw -= dx * 0.01f;
        Pitch = Math.Clamp(Pitch + dy * 0.01f, 0.15f, 1.53f);
    }

    public void Zoom(float factor) => Distance = Math.Clamp(Distance * factor, 40, 30000);

    /// Zoom around the ground under `point`: it stays under the cursor. Off the terrain it zooms
    /// around the middle of the view.
    public void Zoom(float factor, Vector2 point, Vector2 size, MapScene scene)
    {
        if (GroundPoint(point, size, scene) is not { } before)
        {
            Zoom(factor);
            return;
        }
        Zoom(factor);
        // The ray through the point now meets the level of that ground point elsewhere; moving the
        // camera sideways by the difference brings it back exactly.
        var (origin, direction) = Ray(point, size);
        if (MathF.Abs(direction.Z) <= 1e-4f) return;
        var after = origin + direction * ((before.Z - origin.Z) / direction.Z);
        Target.X += before.X - after.X;
        Target.Y += before.Y - after.Y;
    }

    /// Where the ray through a point of the view meets the terrain: marched over the height map,
    /// then refined by bisection. Null when it misses the map.
    public Vector3? GroundPoint(Vector2 point, Vector2 size, MapScene scene)
    {
        if (scene.IsEmpty) return null;
        var (origin, direction) = Ray(point, size);
        bool Above(float t)
        {
            var p = origin + direction * t;
            return p.Z > scene.GroundHeight(p.X, p.Y);
        }
        var step = MathF.Max(Distance / 400, 2);
        float t = 0;
        float? hit = null;
        while (t < Distance * 8 + 5000)
        {
            if (!Above(t + step))
            {
                hit = t + step;
                break;
            }
            t += step;
        }
        if (hit is not { } high) return null;
        var low = t;
        for (int i = 0; i < 24; ++i)
        {
            var middle = (low + high) / 2;
            if (Above(middle)) low = middle;
            else high = middle;
        }
        var p = origin + direction * high;
        var extent = scene.Extent;
        if (p.X < extent.X || p.Y < extent.Y || p.X > extent.Z || p.Y > extent.W) return null;
        return new Vector3(p.X, p.Y, scene.GroundHeight(p.X, p.Y));
    }

    /// Where a world point shows in the view; null behind the camera.
    public Vector2? ViewPoint(Vector3 world, Vector2 size)
    {
        var p = Vector4.Transform(new Vector4(world, 1), ViewProjection(size.X / MathF.Max(size.Y, 1)));
        if (p.W <= 0) return null;
        return new Vector2((p.X / p.W + 1) / 2 * size.X, (1 - p.Y / p.W) / 2 * size.Y);
    }

    /// World ray through a point of the view.
    public (Vector3 Origin, Vector3 Direction) Ray(Vector2 point, Vector2 size)
    {
        var x = point.X / MathF.Max(size.X, 1) * 2 - 1;
        var y = 1 - point.Y / MathF.Max(size.Y, 1) * 2;
        Matrix4x4.Invert(ViewProjection(size.X / MathF.Max(size.Y, 1)), out var inverse);
        Vector3 Unproject(float z)
        {
            var p = Vector4.Transform(new Vector4(x, y, z, 1), inverse);
            return new Vector3(p.X, p.Y, p.Z) / p.W;
        }
        var near = Unproject(0);
        var far = Unproject(1);
        return (near, Vector3.Normalize(far - near));
    }
}

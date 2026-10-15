# Environment maps

Equirectangular Radiance `.hdr` files used by the path tracer's "Use HDRI" option.
Pick one in the Render panel, or load your own with the path field or `--hdri file.hdr`.

| File | Source | Licence |
|---|---|---|
| `venice_sunset_1k.hdr` | [Poly Haven](https://polyhaven.com/a/venice_sunset) | CC0 |
| `kloofendal_48d_partly_cloudy_puresky_1k.hdr` | [Poly Haven](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) | CC0 |
| `studio_small_09_1k.hdr` | [Poly Haven](https://polyhaven.com/a/studio_small_09) | CC0 |

The tracer lights the scene from the map by BSDF sampling only. A small, very bright sun in a map
is therefore noisy; the Clamp slider caps it.

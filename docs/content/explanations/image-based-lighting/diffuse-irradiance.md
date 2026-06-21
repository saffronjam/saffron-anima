+++
title = 'Diffuse irradiance'
weight = 4
math = true
+++

# Diffuse irradiance

Diffuse irradiance is the total light arriving at a surface from the whole environment, weighted by the cosine of the angle to the surface normal $n$. It is the diffuse half of image-based lighting: the answer to how much ambient light a matte surface receives from its surroundings.

A Lambertian surface scatters incoming light equally in all directions, so the irradiance depends only on $n$, not on the view direction. This lets it precompute into a small cube indexed by normal — one convolution at bake time, a single fetch per fragment.

## The integral

Irradiance is the environment integrated over the hemisphere around $n$, weighted by the cosine $\cos\theta = n\cdot l$:

$$
E(n) = \int_\Omega L_i(l)\, (n \cdot l)\, dl
$$

The Lambertian BRDF turns irradiance into outgoing diffuse radiance by $\rho/\pi$. The mesh shader therefore multiplies the sampled irradiance by `albedo`, and the $1/\pi$ is folded into the bake constant described below.

## How the convolution samples the hemisphere

Each output texel corresponds to a normal $n$. The shader builds a tangent frame around it, then walks a regular grid in spherical coordinates: 64 steps in azimuth $\phi$ (`phiSteps`), 16 in zenith $\theta$ (`thetaSteps`). Each sample is weighted by `cos(theta) * sin(theta)`.

```hlsl
float3 sampleDir = tangent.x * right + tangent.y * up + tangent.z * n;
irradiance += envCube.SampleLevel(sampleDir, 0.0).rgb * cos(theta) * sin(theta);
```

The $\cos\theta$ is the Lambert cosine. The $\sin\theta$ is the Jacobian of the spherical parameterization; without it, samples bunch up near the pole and over-count. A `+ 0.5` offset puts samples at cell centers.

## Normalizing the sum

A uniform-grid average is not a Monte-Carlo estimator of the integral, so the result is scaled to keep energy correct:

```hlsl
irradiance = PI * irradiance / float(samples);
```

Dividing by the sample count averages the samples. The leading $\pi$ accounts for the analytic value of $\int_\Omega \cos\theta\sin\theta\,d\theta\,d\phi$ over the hemisphere. The mesh shader writes `kd * irradiance * albedo`, so the $1/\pi$ of the Lambertian term already lives in this bake constant and is not applied again.

## Building the tangent frame

The convolution needs an orthonormal basis $(\text{right}, \text{up}, n)$ to orient the hemisphere. The shader picks an up vector not parallel to $n$, then applies Gram-Schmidt:

```hlsl
float3 up    = abs(n.y) < 0.999 ? float3(0,1,0) : float3(1,0,0);
float3 right = normalize(cross(up, n));
up = cross(n, right);
```

The `0.999` guard swaps to a different reference axis near the poles, where the world up would be degenerate — exactly the sky-poles of a cube.

## Why a tiny cube is enough

The cosine convolution is a heavy low-pass filter. It removes all high-frequency detail, so the output is smooth no matter how sharp the environment is. A `32²` cube captures it with no visible loss, which is why [the bake](../ibl-bake-pass/) keeps it that small. The whole convolution is `32 × 32 × 6 × 1024` environment samples, done once at startup.

## In the code

| What | File | Symbols |
|---|---|---|
| Convolution + tangent frame | `engine/assets/shaders/ibl_irradiance.slang` | `computeMain`, `phiSteps`/`thetaSteps`, `PI / samples` normalize, `up`/`right` |
| Bindings | `engine/assets/shaders/ibl_irradiance.slang` | `envCube` (sampler), `outCube` (storage) |
| Cube size | `engine/crates/rendering/src/ibl.rs` | `IBL_IRRADIANCE_SIZE` (32) |
| Consumed as diffuse | `engine/assets/shaders/lighting.slang` | ambient block — `kd * irradiance * albedo` |

## Related

- [Procedural sky](../procedural-sky/) — the environment being convolved
- [Specular prefilter](../specular-prefilter/) — the specular counterpart
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — the Lambertian diffuse term this feeds

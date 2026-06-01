+++
title = 'SaffronEngine'
description = 'How the SaffronEngine renderer works, concept by concept, with links into the code that implements it.'
+++

# SaffronEngine

A from-scratch **Vulkan** renderer and **C++26** game engine with an ImGui editor.
These docs explain how this engine renders, concept by concept, and link each
explanation to the code behind it.

The code is Go-flavoured: small data structs, free functions, errors as values, no
inheritance. That style runs through everything, so read [the conventions](explanations/core-and-conventions/)
before the deep graphics pages.

## Where to start

- **New here?** The [Overview](overview/) is a one-screen tour from window to pixel.
- **A subsystem?** [Explanations](explanations/) covers each part in its own page — render graph, lighting, shadows, post-processing, the editor.
- **Getting something done?** [How-to](how-to/) has task recipes (build and run, import a model, drive the `se` CLI).
- **A signature or command name?** [Reference](reference/) is the flat catalog.
- **Learning by doing?** [Tutorials](tutorials/) walk through complete tasks end to end.

## How these docs are organised

The four sections follow the [Diátaxis](https://diataxis.fr/) framework: explanations
say how and why something works, how-tos are recipes, reference is lookup, tutorials
are guided builds.

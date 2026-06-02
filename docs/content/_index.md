+++
title = 'SaffronEngine'
description = 'How the SaffronEngine renderer works, concept by concept, with links into the code that implements it.'
+++

# SaffronEngine

SaffronEngine is a from-scratch **Vulkan** renderer and **C++26** game engine with a
Tauri/React editor. These docs explain how it renders, concept by concept, and link
each explanation to the code behind it.

The code is Go-flavoured: small data structs, free functions, errors as values, no
inheritance. That style runs through the whole engine, so read [the conventions](explanations/core-and-conventions/)
before the deeper graphics pages.

## Where to start

- **Overview:** the [Overview](overview/) is a one-screen tour from window to pixel.
- **Subsystems:** [Explanations](explanations/) covers each part in its own page — render graph, lighting, shadows, post-processing, the editor.
- **Tasks:** [How-to](how-to/) has recipes (build and run, import a model, drive the `se` CLI).
- **Signatures and commands:** [Reference](reference/) is the flat catalog.
- **Guided builds:** [Tutorials](tutorials/) walk through complete tasks end to end.

## How these docs are organised

The four sections follow the [Diátaxis](https://diataxis.fr/) framework: explanations
cover how and why something works, how-tos are recipes, reference is lookup, and
tutorials are guided builds.

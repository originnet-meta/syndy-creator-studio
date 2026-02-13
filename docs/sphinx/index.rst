.. OBS Studio documentation master file, created by
   sphinx-quickstart on Wed Oct 25 00:03:21 2017.
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

..
   /******************************************************************************
       Modifications Copyright (C) 2026 Uniflow, Inc.
       Author: Kim Taehyung <gaiaengine@gmail.com>
       Modified: 2026-02-12
       Notes: Changes for Syndy Creator Studio.
   ******************************************************************************/

.. _contents:

Welcome to SYNDY Creator Studio's documentation!
================================================

* **Setting up an SYNDY Creator Studio development environment?** :wiki:`Use the Wiki <Install-Instructions>`
* **Developing your first SYNDY Creator Studio plugin?** `Use the obs-plugintemplate <https://github.com/obsproject/obs-plugintemplate#obs-plugin-template>`_

.. toctree::
   :caption: Core Concepts
   :maxdepth: 1

   backend-design
   plugins
   frontends
   graphics
   scripting

.. toctree::
   :caption: API Reference
   :maxdepth: 2

   OBS Core <reference-core>
   Canvases <reference-canvases>
   Modules <reference-modules>
   Core API Object <reference-core-objects>
   Platform/Utility <reference-libobs-util>
   Callbacks (libobs/callback) <reference-libobs-callback>
   Graphics (libobs/graphics) <reference-libobs-graphics>
   Media I/O (libobs/media-io) <reference-libobs-media-io>
   reference-frontend-api

.. toctree::
   :caption: Additional Resources
   :maxdepth: 1
   :hidden:

   Build Instructions <https://obsproject.com/wiki/Install-Instructions>
   Plugin Template <https://github.com/obsproject/obs-plugintemplate#obs-plugin-template>

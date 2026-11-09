# cute-AO

Ambient Occlusion from depth map for AE. high bit depth recommended, but can work with even compressed footage

Apply **cute-AO** to a solid or footage layer, then select a grayscale depth pass with **Depth Layer**. For ordinary recorded grayscale depth, keep **Depth Encoding** set to **Linear**. For compressed footage, start with **Depth Smoothing** around `0.5–1.0`.

The plug-in preserves the original NeoSSAO controls and adds depth encoding and smoothing controls for After Effects footage. It supports 8-, 16-, and 32-bpc projects. This CPU port is slower than the original GPU shader; start with Render Resolution `0.5`, Ray Steps `8`, and Slice Count `4`.

## Install

Copy `cute-AO.aex` into the After Effects Plug-ins folder and restart After Effects. The effect appears under **Effect > Stylize > cute-AO**.

## Build

Visual Studio 2022 with Desktop development with C++ and the Adobe After Effects SDK are required.

```powershell
.\build.ps1 -AESDKRoot "C:\path\to\AfterEffectsSDK"
```

## Attribution

Derived from `BaBa_NeoSSAO.fx` 2.2 by Barbatos, licensed under the MIT License.

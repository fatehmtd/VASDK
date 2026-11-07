# Third-party notices for <App name>

<App name> is built on [VASDK](https://github.com/fatehmtd/VASDK), licensed under the [Apache License 2.0](https://github.com/fatehmtd/VASDK/blob/master/LICENSE). It uses the libraries, models and services below, each under its own license. Read the restrictions first.

## Restrictions

<Only what applies to this app; delete the rest.>

- **Non-commercial:** <model> (<license>) may not be used commercially. Swap it for <alternative> before shipping a commercial product.
- **Use-restricted:** <model> (<OpenRAIL>) comes with use restrictions: [license](<url>).
- **Cloud services:** <service> is used under its own terms of service: [terms](<url>). Audio or text is sent to it.

## Models

| Model | Used for | Source | License |
| --- | --- | --- | --- |
| <model> | <stage> (`<provider id>`) | [<repo>](<url>) | [<license>](<license url>) |

## Libraries

| Library | Used for | Source | License |
| --- | --- | --- | --- |
| <library> | <what> (`vasdk_<target>`) | [<repo>](<url>) | [<license>](<license url>) |

## Services

| Service | Used for | Terms |
| --- | --- | --- |
| <service> | <STT / TTS / LLM> | [<terms>](<url>) |

<Generated from the providers in `config/engine.txtpb`, the `vasdk_*` targets in `CMakeLists.txt`, and the root THIRD_PARTY_NOTICES.md of VASDK at commit `<sha>`.>

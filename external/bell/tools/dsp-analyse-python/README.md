# dsp-analyse

Simple python plotter for the bell-dsp lib. To run it, make sure you have first compiled the shared library in the "build" folder.

TL:DR: `mkdir build && cmake .. -GNinja && ninja`

The python deps are managed with poetry.

Example usage:
`poetry run plot-pipeline pipelines/basic-gain-lowshelf.json`
#!/bin/bash
set -ev

#conda install anaconda-client
anaconda -t ${CONDA_UPLOAD_TOKEN} upload -u ${CONDA_USER} -l main -l edge ~/miniconda/conda-bld/linux-64/loom-*.tar.bz2 --force

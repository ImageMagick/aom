#!/usr/bin/env python3
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "Tools"))

from dependency_updater import Step, UpdateError, Updater

URL = "https://aomedia.googlesource.com/aom/+archive/v{version}.tar.gz"

CONFIG_FILES = [
    "aom_config.h",
    "aom_dsp_rtcd.h",
    "aom_scale_rtcd.h",
    "aom_version.h",
    "av1_rtcd.h",
]


def update_source(updater):
    updater.replace_source()
    updater.update_config()


def create_config_files(updater):
    updater.cmake("-DAOM_TARGET_CPU=generic")
    updater.cmake_build("aom_rtcd")
    updater.copy_from_build("config", CONFIG_FILES, "config")
    updater.remove_build()
    version = updater.read_file("config/aom_version.h")
    if f'#define VERSION_STRING " v{updater.version}"' not in version:
        raise UpdateError(f"config/aom_version.h does not contain version {updater.version}")


def move_header(updater):
    weights_path = "av1/encoder/partition_cnn_weights.h"
    weights = updater.read_file(weights_path)
    match = re.search(r"^(.*?#include \"av1/encoder/ml\.h\"\n\n)(#define CNN_BRANCH_0_OUT_CH .*?\n\n)(?=#define NUM_DNN_BRANCHES )",
        weights, re.S | re.M)
    if not match:
        raise UpdateError(f"unable to find the CNN defines in {weights_path}")
    header = match.group(1).replace("AOM_AV1_ENCODER_PARTITION_CNN_WEIGHTS_H_", "AOM_AV1_ENCODER_PARTITION_CNN_H_")
    updater.write_file("av1/encoder/partition_cnn.h",
        header + match.group(2) +
        "#ifdef __cplusplus\n"
        "}  // extern \"C\"\n"
        "#endif\n"
        "\n"
        "#endif  // AOM_AV1_ENCODER_PARTITION_CNN_H_\n")
    updater.replace_in_file(weights_path, match.group(2), "")
    updater.replace_in_file("av1/encoder/block.h",
        "#include \"av1/encoder/partition_cnn_weights.h\"",
        "#include \"av1/encoder/partition_cnn.h\"")


updater = Updater(__file__, "aom", URL)
updater.run([
    Step("Replace the source with the release and update Config.txt", update_source,
        "Updated aom to version {version}"),
    Step("Use cmake to create the config files", create_config_files,
        "Use cmake to create the config files."),
    Step("Move part of the header to a separate file to reduce the binary size", move_header,
        "Move part of the header to a separate file to reduce the binary size."),
    updater.clone_dependencies_step(),
])

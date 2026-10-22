"""datasets/ 数据案例：Python 接口读取、统计与选择语言。

这些用例同时充当「数据案例」的回归测试：新增/替换数据集时，原子数、
帧数或配体选择数量变化会立刻失败，提醒同步 `datasets/README.md` 与文档。
"""

from __future__ import annotations

from collections import Counter
from pathlib import Path

import pytest

import moleculens as ml

DATASETS = Path(__file__).resolve().parents[2] / "datasets"

# (相对路径, 原子数, 键数, 帧数)
CASES = [
    ("pdb/1CRN.pdb", 327, 3, 1),
    ("pdb/1UBQ.pdb", 660, 608, 1),
    ("pdb/1BNA.pdb", 566, 545, 1),
    ("pdb/4HHB.pdb", 4779, 204, 1),
    ("pdb/1L2Y.pdb", 304, 310, 38),
    ("pdb/1HVR.pdb", 1890, 72, 1),
    ("pdb/1U19.pdb", 5948, 418, 1),
    ("pubchem/aspirin_2244.sdf", 21, 21, 1),
    ("pubchem/caffeine_2519.sdf", 24, 25, 1),
    ("pubchem/ibuprofen_3672.sdf", 33, 33, 1),
    ("pubchem/ethanol_702.sdf", 9, 8, 1),
    ("pubchem/benzene_241.sdf", 12, 12, 1),
    ("pubchem/glucose_5793.sdf", 24, 24, 1),
    ("pubchem/resveratrol_445154.sdf", 29, 30, 1),
    ("md/md_1u19.gro", 5547, 5284, 1),
    ("esp/glycine/glycine.pdb", 10, 10, 1),
    ("esp/glycine/glycine_density.cube", 10, 10, 1),
    ("esp/glycine/glycine_esp.cube", 10, 10, 1),
]


@pytest.mark.parametrize("relpath,atoms,bonds,frames", CASES)
def test_dataset_summary(relpath: str, atoms: int, bonds: int, frames: int) -> None:
    path = DATASETS / relpath
    if not path.is_file():
        pytest.skip(f"dataset missing: {path}")
    structure = ml.load(path)
    summary = structure.summary()
    assert summary["atom_count"] == atoms, relpath
    assert summary["bond_count"] == bonds, relpath
    assert summary["frame_count"] == frames, relpath
    assert len(structure.atoms) == atoms
    assert len(structure.bonds) == bonds


def test_protein_ligand_selection() -> None:
    path = DATASETS / "pdb" / "1HVR.pdb"
    if not path.is_file():
        pytest.skip("1HVR missing")
    structure = ml.load(path)
    ligand = structure.select("resname XK2")
    assert len(ligand) == 46
    assert len(set(ligand)) == 46
    pocket = structure.select("within 6 of resname XK2")
    assert len(pocket) == 260
    assert set(ligand).issubset(set(pocket))


def test_rhodopsin_ligand_and_glycans() -> None:
    path = DATASETS / "pdb" / "1U19.pdb"
    if not path.is_file():
        pytest.skip("1U19 missing")
    structure = ml.load(path)
    assert len(structure.select("resname RET")) == 40
    assert len(structure.select("resname NAG or resname MAN or resname BMA")) == 145
    # 视黄醛只出现在配体里，不应与蛋白主链混淆
    assert {structure.atoms[i].residue_name for i in structure.select("resname RET")} == {"RET"}


def test_nmr_multimodel_frames() -> None:
    path = DATASETS / "pdb" / "1L2Y.pdb"
    if not path.is_file():
        pytest.skip("1L2Y missing")
    structure = ml.load(path)
    assert structure.frame_count == 38
    assert structure.summary()["frame_count"] == 38


def test_small_molecule_composition() -> None:
    path = DATASETS / "pubchem" / "ethanol_702.sdf"
    if not path.is_file():
        pytest.skip("ethanol missing")
    structure = ml.load(path)
    assert Counter(atom.element for atom in structure) == Counter({"C": 2, "H": 6, "O": 1})
    assert structure.summary()["composition"] == {"C": 2, "H": 6, "O": 1}


def test_info_matches_summary() -> None:
    path = DATASETS / "pdb" / "1CRN.pdb"
    if not path.is_file():
        pytest.skip("1CRN missing")
    assert ml.info(path)["atom_count"] == ml.load(path).summary()["atom_count"]


def test_manifest_exists() -> None:
    manifest = DATASETS / "README.md"
    if not manifest.is_file():
        pytest.skip("datasets/README.md missing")
    text = manifest.read_text(encoding="utf-8")
    for relpath, *_ in CASES:
        assert relpath in text, f"{relpath} not listed in datasets/README.md"

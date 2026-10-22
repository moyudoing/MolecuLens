"""格式解析测试：PDB / GRO / SDF / cube / 多帧 PDB 的统计与数值约定。"""

from __future__ import annotations

from collections import Counter
from pathlib import Path

import pytest

import moleculens as ml


def test_pdb_counts_and_composition(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    assert structure.atom_count == 35
    assert structure.bond_count == 37
    assert structure.structure_count == 1
    assert structure.instance_count == 1
    assert structure.frame_count == 1
    assert Counter(atom.element for atom in structure) == {
        "C": 15,
        "H": 14,
        "O": 6,
    }
    assert Counter(atom.element for atom in structure) == Counter(
        structure.summary()["composition"]
    )


def test_pdb_coordinates_are_finite_and_local(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    for atom in structure:
        assert -1000.0 < atom.x < 1000.0
        assert -1000.0 < atom.y < 1000.0
        assert -1000.0 < atom.z < 1000.0
    xs = [atom.x for atom in structure]
    assert max(xs) - min(xs) < 50.0  # 小分子，不应出现跨盒子的异常坐标


def test_glycine_pdb_metadata_and_inference(glycine_path: Path) -> None:
    structure = ml.load(glycine_path)
    assert structure.atom_count == 10
    assert structure.bond_count == 10  # 无 CONECT → 距离推断

    assert Counter(atom.name for atom in structure) == {
        "H": 5,
        "C": 2,
        "O": 2,
        "N": 1,
    }
    # 注：data/glycine.pdb 的列位比 PDB 规范少一个空格（残基名落在 17–19 列），
    # 解析器按规范列位读取，因此这里不断言 residue_name / chain_name；
    # 这两个字段的语义由 md.gro 用例覆盖。

    without_bonds = ml.load(glycine_path, infer_bonds=False)
    assert without_bonds.atom_count == 10
    assert without_bonds.bond_count == 0


def test_gro_matches_pdb_convention(epi_path: Path, data_dir: Path) -> None:
    gro_path = data_dir / "EPI.gro"
    if not gro_path.is_file():
        pytest.skip(f"fixture missing: {gro_path}")
    pdb = ml.load(epi_path)
    gro = ml.load(gro_path)
    assert gro.atom_count == pdb.atom_count == 35
    assert [atom.element for atom in gro] == [atom.element for atom in pdb]
    assert [atom.name for atom in gro] == [atom.name for atom in pdb]
    # GRO 以 nm 记录；加载器统一换算为 Å（三位小数，1 nm 精度 → 0.01 Å 误差）
    for left, right in zip(gro, pdb):
        assert left.x == pytest.approx(right.x, abs=0.02)
        assert left.y == pytest.approx(right.y, abs=0.02)
        assert left.z == pytest.approx(right.z, abs=0.02)


def test_large_gro_expands_instances(md_gro_path: Path) -> None:
    structure = ml.load(md_gro_path)
    assert structure.atom_count == 6452
    assert structure.bond_count == 4436
    assert structure.structure_count == 7
    assert structure.instance_count == 2042
    assert len({atom.instance for atom in structure}) == 2042

    assert structure.atoms[0].instance == 0
    assert structure.atoms[-1].instance == 2041
    assert structure.atoms[0].residue_name == "API"
    assert structure.atoms[0].chain_name == "API"
    assert {"SOL", "API"} <= {atom.residue_name for atom in structure}

    for bond in structure.bonds:
        assert bond.a < structure.atom_count
        assert bond.b < structure.atom_count


def test_sdf_records_parse_warnings(epi_path: Path, data_dir: Path) -> None:
    sdf_path = data_dir / "(+) - Epicatechin.sdf"
    if not sdf_path.is_file():
        pytest.skip(f"fixture missing: {sdf_path}")
    structure = ml.load(sdf_path)
    assert structure.atom_count == 35
    assert structure.bond_count == 37
    assert len(structure.warnings) == 1
    warning = structure.warnings[0]
    assert isinstance(warning, ml.Warning)
    assert warning.line == 80
    assert "V2000" in warning.message
    assert structure.summary()["warnings"][0]["line"] == 80


def test_cube_imports_header_atoms_only(repo_root: Path) -> None:
    cube_path = repo_root / "datasets" / "esp" / "glycine" / "glycine_density.cube"
    if not cube_path.is_file():
        pytest.skip(f"fixture missing: {cube_path}")
    structure = ml.load(cube_path)
    assert structure.atom_count == 10
    assert structure.bond_count == 10
    assert len(structure.warnings) == 1
    assert "体积数据" in structure.warnings[0].message
    assert "Bohr" in structure.warnings[0].message


def test_multiframe_pdb_reports_frame_count(data_dir: Path) -> None:
    path = data_dir / "traj_multiframe.pdb"
    if not path.is_file():
        pytest.skip(f"fixture missing: {path}")
    structure = ml.load(path)
    assert structure.atom_count == 4
    assert structure.bond_count == 4
    assert structure.frame_count == 3
    assert structure.dt == pytest.approx(2.0)

"""公共包接口测试：版本、元素表、Structure 容器协议与路径处理。"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

import moleculens as ml


def test_version_is_exposed() -> None:
    assert isinstance(ml.__version__, str)
    assert ml.__version__.count(".") >= 1
    assert ml.__version__ != "0.0.0"


def test_supported_extensions_cover_builtin_parsers() -> None:
    extensions = ml.supported_extensions()
    assert isinstance(extensions, list)
    for expected in ("pdb", "ent", "gro", "sdf", "mol", "cube", "cub"):
        assert expected in extensions
    assert len(extensions) == len(set(extensions))


def test_element_info_by_symbol_and_number() -> None:
    carbon = ml.element_info("C")
    assert carbon.symbol == "C"
    assert carbon.name == "Carbon"
    assert carbon.atomic_number == 6
    assert carbon.mass == pytest.approx(12.011, abs=1e-3)
    assert carbon.vdw_radius == pytest.approx(1.70)
    assert carbon.covalent_radius == pytest.approx(0.76)
    assert len(carbon.color) == 3

    assert ml.element_info(8).symbol == "O"
    assert ml.element_info("o").symbol == "O"    # 大小写不敏感
    assert ml.element_info(" cl ").symbol == "Cl"

    with pytest.raises(ValueError):
        ml.element_info("Zz")
    with pytest.raises(ValueError):
        ml.element_info(0)
    with pytest.raises(ValueError):
        ml.element_info(119)


def test_elements_table_has_118_unique_symbols() -> None:
    table = ml.elements()
    assert len(table) == 118
    symbols = [element.symbol for element in table]
    assert len(symbols) == len(set(symbols))
    assert symbols[0] == "H"
    assert symbols[-1] == "Og"
    assert {element.atomic_number for element in table} == set(range(1, 119))


def test_structure_container_protocol(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    assert isinstance(structure, ml.Structure)
    assert len(structure) == structure.atom_count == 35
    assert structure.bond_count == 37

    atoms = structure.atoms
    assert isinstance(atoms, list)
    assert len(atoms) == len(structure)
    assert [atom.index for atom in atoms] == list(range(35))
    assert list(structure) == atoms

    assert structure[0].name == "O01"
    assert structure[-1] == atoms[-1]
    assert structure[0] == structure.atom(0)
    assert structure[0] != structure[1]
    assert hash(structure[0]) == hash(atoms[0])
    assert structure[0] in {atoms[0]}
    with pytest.raises(IndexError):
        _ = structure[35]
    with pytest.raises(IndexError):
        _ = structure[-36]


def test_atom_fields_are_populated(epi_path: Path) -> None:
    atom = ml.load(epi_path).atoms[0]
    assert atom.index == 0
    assert atom.instance == 0
    assert atom.name == "O01"
    assert atom.element == "O"
    assert atom.element_name == "Oxygen"
    assert atom.atomic_number == 8
    assert atom.residue_name == "EP"
    assert atom.residue_id == 0
    assert atom.chain_id == 0
    assert atom.occupancy == pytest.approx(100.0)
    assert atom.bfactor == pytest.approx(0.0)
    assert atom.position == pytest.approx((0.455, -0.756, -0.112), abs=1e-3)
    assert atom.mass == pytest.approx(15.999, abs=1e-3)
    assert len(atom.color) == 3


def test_bonds_are_in_range_and_valid(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    assert structure.bonds
    for bond in structure.bonds:
        assert 0 <= bond.a < len(structure)
        assert 0 <= bond.b < len(structure)
        assert bond.a < bond.b
        assert bond.order in (0, 1, 2, 3)


def test_structure_repr_and_summary(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    text = repr(structure)
    assert "moleculens.Structure" in text
    assert "atoms=35" in text
    assert "bonds=37" in text

    summary = structure.summary()
    assert summary["source"] == str(epi_path)
    assert summary["atom_count"] == 35
    assert summary["bond_count"] == 37
    assert summary["structure_count"] == 1
    assert summary["instance_count"] == 1
    assert summary["frame_count"] == 1
    assert summary["dt"] == pytest.approx(2.0)
    assert summary["composition"] == {"C": 15, "H": 14, "O": 6}
    assert summary["warnings"] == []

    assert repr(structure.atoms[0]).startswith("<moleculens.Atom 0 O01 (O) EP>")
    assert repr(structure.bonds[0]).startswith("<moleculens.Bond ")


def test_facade_info_matches_summary(epi_path: Path) -> None:
    assert ml.info(epi_path) == ml.load(epi_path).summary()


def test_path_types_are_accepted(epi_path: Path) -> None:
    from_pathlib = ml.load(epi_path)
    from_str = ml.load(str(epi_path))
    from_bytes = ml.load(os.fsencode(epi_path))
    assert from_pathlib.atom_count == from_str.atom_count == from_bytes.atom_count == 35
    assert from_str.atoms[0].name == "O01"


def test_positions_property_matches_atoms(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    positions = structure.positions
    assert len(positions) == len(structure)
    for atom, position in zip(structure, positions):
        assert position == (atom.x, atom.y, atom.z)


def test_missing_file_raises_file_not_found(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        ml.load(tmp_path / "does-not-exist.pdb")


def test_directory_path_is_rejected(tmp_path: Path) -> None:
    with pytest.raises(IsADirectoryError):
        ml.load(tmp_path)


def test_wrong_path_type_raises_type_error() -> None:
    with pytest.raises(TypeError):
        ml.load(123)  # type: ignore[arg-type]

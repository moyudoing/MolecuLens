"""选择语言测试：与 Python 侧独立计算的结果逐项比对。

原生实现见 src/entity/selection/selection.cpp；这里不复用它的逻辑，
而是按文档语义（属性比较 / AND / OR / NOT / within / same residue）
用原子快照重新算一遍，防止「绑定层漏改而 C++ 测试仍通过」。
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable, Iterable

import pytest

import moleculens as ml


def _indices(structure: ml.Structure, predicate: Callable[[ml.Atom], bool]) -> list[int]:
    return [atom.index for atom in structure if predicate(atom)]


def _within(structure: ml.Structure, reference: Iterable[int], distance: float) -> list[int]:
    reference = list(reference)
    limit = distance * distance
    selected: set[int] = set()
    for i in reference:
        source = structure.atoms[i]
        for atom in structure:
            dx = atom.x - source.x
            dy = atom.y - source.y
            dz = atom.z - source.z
            if dx * dx + dy * dy + dz * dz <= limit:
                selected.add(atom.index)
    return sorted(selected)


def _same_residue(structure: ml.Structure, reference: Iterable[int]) -> list[int]:
    keys = {
        (structure.atoms[i].instance, structure.atoms[i].residue_id,
         structure.atoms[i].chain_id)
        for i in reference
    }
    return [
        atom.index
        for atom in structure
        if (atom.instance, atom.residue_id, atom.chain_id) in keys
    ]


def test_all_and_none(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    assert structure.select("all") == list(range(len(structure)))
    assert structure.select("none") == []


def test_attribute_comparisons_match_python_filter(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    assert structure.select("element C") == _indices(
        structure, lambda atom: atom.element == "C"
    )
    assert structure.select("element = C") == structure.select("element C")
    assert structure.select("element c") == structure.select("element C")

    assert structure.select("name O01") == _indices(
        structure, lambda atom: atom.name == "O01"
    )
    assert structure.select("resname ep") == _indices(
        structure, lambda atom: atom.residue_name == "EP"
    )
    assert structure.select("resid 0") == list(range(len(structure)))
    assert structure.select("resid > 0") == []
    assert structure.select("occupancy = 100") == list(range(len(structure)))
    assert structure.select("bfactor < 50") == list(range(len(structure)))


def test_boolean_operators(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    carbon = set(structure.select("element C"))
    hydrogen = set(structure.select("element H"))
    oxygen = set(structure.select("element O"))

    assert set(structure.select("element C or element H")) == carbon | hydrogen
    assert set(structure.select("element C and element C")) == carbon
    assert structure.select("element C and element O") == []
    assert set(structure.select("not element H")) == set(range(len(structure))) - hydrogen
    assert set(structure.select("(element C or element O) and not element O")) == carbon
    assert set(structure.select("not (element C or element H or element O)")) == set()

    # 关键字/运算符大小写不敏感（属性名保持小写）
    assert set(structure.select("element C OR element O")) == carbon | oxygen


def test_within_uses_loaded_coordinates(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    oxygen = structure.select("element O")
    assert oxygen
    assert structure.select("within 1.9 of element O") == _within(
        structure, oxygen, 1.9
    )
    # 距离 0 的选择至少包含参考原子本身
    zero = structure.select("within 0 of element O")
    assert set(oxygen) <= set(zero)


def test_same_residue_on_multi_instance_fixture(md_gro_path: Path) -> None:
    structure = ml.load(md_gro_path)
    reference = structure.select("name C05")
    if not reference:
        pytest.skip("fixture has no C05 atom")
    expected = _same_residue(structure, reference)
    assert structure.select("same residue as name C05") == expected
    assert set(reference) <= set(expected)


def test_chain_and_resname_on_multi_instance_fixture(md_gro_path: Path) -> None:
    structure = ml.load(md_gro_path)
    assert structure.select("resname SOL") == _indices(
        structure, lambda atom: atom.residue_name == "SOL"
    )
    assert structure.select("chain API") == _indices(
        structure, lambda atom: atom.chain_name == "API"
    )


def test_selection_object_exposes_mask_and_indices(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    selection = structure.selection("element C")
    assert isinstance(selection, ml.Selection)
    assert selection.expression == "element C"
    assert selection.count == 15
    assert len(selection) == 15
    assert selection.indices == structure.select("element C")
    assert len(selection.mask) == len(structure)
    assert sum(selection.mask) == 15
    assert selection.mask[6] is True
    assert selection.mask[0] is False
    assert 6 in selection
    assert 0 not in selection
    assert -1 not in selection
    assert "15 atoms" in repr(selection)

    empty = structure.selection("none")
    assert empty.count == 0
    assert empty.indices == []
    assert not any(empty.mask)


def test_placeholder_words_select_nothing(epi_path: Path) -> None:
    """protein/backbone/sidechain/waters 是已注册但未实现的占位宏。"""
    structure = ml.load(epi_path)
    for word in ("protein", "backbone", "sidechain", "waters"):
        assert structure.select(word) == []


@pytest.mark.parametrize(
    "expression",
    [
        "",
        "element",
        "element =",
        "within 2 element O",
        "within 2 of",
        "same residue as",
        "unknown_attribute 3",
        "AND",
        "(",
    ],
)
def test_invalid_expressions_raise_value_error(epi_path: Path, expression: str) -> None:
    structure = ml.load(epi_path)
    with pytest.raises(ValueError):
        structure.select(expression)
    with pytest.raises(ValueError):
        structure.selection(expression)


def test_select_returns_sorted_unique_indices(epi_path: Path) -> None:
    structure = ml.load(epi_path)
    indices = structure.select("element C or element O")
    assert indices == sorted(set(indices))
    assert all(isinstance(index, int) for index in indices)
    assert all(0 <= index < len(structure) for index in indices)

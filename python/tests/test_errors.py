"""错误处理与边界路径测试。"""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

import moleculens as ml


def test_unsupported_extension_raises_moleculens_error(tmp_path: Path) -> None:
    path = tmp_path / "not_a_structure.xyz"
    path.write_text("this is not a molecular structure\n", encoding="utf-8")
    with pytest.raises(ml.MoleculensError) as excinfo:
        ml.load(path)
    message = str(excinfo.value)
    assert "FormatError" in message or "ParseError" in message
    assert "not_a_structure.xyz" in message


def test_malformed_pdb_reports_parse_error(tmp_path: Path) -> None:
    path = tmp_path / "broken.pdb"
    path.write_text("REMARK no atoms here\n", encoding="utf-8")
    with pytest.raises(ml.MoleculensError) as excinfo:
        ml.load(path)
    assert "ParseError" in str(excinfo.value)


def test_unicode_and_spaced_path_works(epi_path: Path, tmp_path: Path) -> None:
    target_dir = tmp_path / "中文 目录"
    target_dir.mkdir()
    target = target_dir / "结构 文件.pdb"
    shutil.copyfile(epi_path, target)

    structure = ml.load(target)
    assert structure.atom_count == 35
    assert structure.source == str(target)


def test_strict_mode_accepts_valid_file(glycine_path: Path) -> None:
    structure = ml.load(glycine_path, strict=True)
    assert structure.atom_count == 10


def test_moleculens_error_is_public_exception() -> None:
    assert issubclass(ml.MoleculensError, Exception)


def test_empty_gro_reports_error(tmp_path: Path) -> None:
    path = tmp_path / "empty.gro"
    path.write_text("Empty\n0\n", encoding="utf-8")
    with pytest.raises(ml.MoleculensError):
        ml.load(path)

"""Regression checks for the durable AI-column system-table definitions.

These checks only validate schema generation. They do not prove that SQL DDL,
post-commit task publication, or the background worker are implemented.
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = ROOT / "src/share/inner_table/generate_inner_table_schema.py"


class AIColumnSchemaDefinitionTest(unittest.TestCase):
    def test_ai_column_metadata_and_task_tables_generate(self):
        with tempfile.TemporaryDirectory(prefix="seekdb-ai-schema-") as directory:
            output = pathlib.Path(directory)
            subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR),
                    "--share-output-dir",
                    str(output / "share"),
                    "--observer-output-dir",
                    str(output / "observer"),
                    "--quiet",
                ],
                cwd=ROOT,
                check=True,
            )
            constants = (
                output / "share/ob_inner_table_schema_constants.h"
            ).read_text(encoding="utf-8")
            generated = (
                output / "share/ob_inner_table_schema.1101_1150.cpp"
            ).read_text(encoding="utf-8")

        self.assertIn("OB_ALL_AI_COLUMN_TID = 1131", constants)
        self.assertIn("OB_ALL_AI_COLUMN_TASK_TID = 1132", constants)
        for required_column in (
            "normalized_expr",
            "dependencies",
            "max_retries",
            "definition_version",
            "state_column_id",
            "request_column_id",
            "result_spec_column_id",
            "rowkey_predicate",
            "request_id",
            "generation_id",
            "retry_count",
            "eligible_ts",
            "claimed_by",
        ):
            with self.subTest(column=required_column):
                self.assertIn('"' + required_column + '"', generated)


if __name__ == "__main__":
    unittest.main()

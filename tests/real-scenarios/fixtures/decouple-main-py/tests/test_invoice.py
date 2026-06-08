import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


class InvoiceCliTests(unittest.TestCase):
    def test_invoice_report_totals_are_stable_after_refactor(self):
        with tempfile.TemporaryDirectory() as td:
            csv_path = Path(td) / "items.csv"
            csv_path.write_text(textwrap.dedent("""\
                sku,qty,unit_price
                A-1,2,3.50
                B-2,1,10.00
            """))
            out = subprocess.check_output(
                [sys.executable, "main.py", str(csv_path), "--tax", "0.10"],
                text=True,
            )
        self.assertIn("INVOICE REPORT", out)
        self.assertIn("A-1: 2 × 3.50 = 7.00", out)
        self.assertIn("B-2: 1 × 10.00 = 10.00", out)
        self.assertIn("Subtotal: 17.00", out)
        self.assertIn("Tax: 1.70", out)
        self.assertIn("Total: 18.70", out)


if __name__ == "__main__":
    unittest.main()

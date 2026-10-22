import re
from pathlib import Path

ROOT = Path(__file__).parents[2]


def quickstart_for_the_readme() -> str:
    lines = (ROOT / "tests/docs/test_quickstart.py").read_text().split("\n")
    text = "\n".join(line for line in lines if "--8<--" not in line)
    text = text.replace("from pathlib import Path\n\n", "", 1)
    text = re.sub(r"ELF = .*\n\n", "", text, count=1)
    return text.replace("popcorn.Elf(ELF)", 'popcorn.Elf("gpio_button.elf")').strip() + "\n"


def test_the_readme_example_is_the_quickstart_test() -> None:
    readme = (ROOT / "README.md").read_text()
    block = re.search(r"```python\n(.*?)```", readme, re.DOTALL)
    assert block is not None
    assert block.group(1) == quickstart_for_the_readme()

"""The shopping list, and kitchen unit conversions."""

import json
import threading
from pathlib import Path

from .recipes import spoken_list


class ShoppingList:
    """Items in the order they were added, kept in a JSON file when one is given."""

    def __init__(self, path=None):
        self._path = Path(path) if path else None
        self._lock = threading.Lock()
        self.items = []
        if self._path and self._path.exists():
            self.items = json.loads(self._path.read_text(encoding="utf-8"))

    def add(self, *items):
        with self._lock:
            added = [item for item in items if item and item.lower() not in (i.lower() for i in self.items)]
            self.items += added
            self._save()
        return added

    def remove(self, item):
        with self._lock:
            before = len(self.items)
            self.items = [i for i in self.items if i.lower() != item.lower()]
            self._save()
            return len(self.items) < before

    def clear(self):
        with self._lock:
            self.items = []
            self._save()

    def summary(self):
        if not self.items:
            return "The shopping list is empty."
        count = len(self.items)
        return f"There {'is' if count == 1 else 'are'} {count} thing{'s' if count != 1 else ''} on the list: {spoken_list(self.items)}."

    def _save(self):
        if self._path:
            self._path.write_text(json.dumps(self.items, indent=2), encoding="utf-8")


# What a unit is in milliliters (volume) or grams (mass), by the names it's said by.
_VOLUME = {"ml": 1, "milliliter": 1, "millilitre": 1, "l": 1000, "liter": 1000, "litre": 1000, "cup": 240,
           "tablespoon": 15, "tbsp": 15, "teaspoon": 5, "tsp": 5, "fluid ounce": 29.57, "fl oz": 29.57, "pint": 473}
_MASS = {"g": 1, "gram": 1, "kg": 1000, "kilogram": 1000, "ounce": 28.35, "oz": 28.35, "pound": 453.6, "lb": 453.6}
# Grams per milliliter, to go between a volume and a mass.
_DENSITY = {"flour": 0.53, "sugar": 0.85, "brown sugar": 0.83, "butter": 0.96, "water": 1.0, "milk": 1.03, "rice": 0.85,
            "olive oil": 0.92, "oil": 0.92, "honey": 1.42, "salt": 1.2, "oats": 0.36, "cocoa": 0.42}


def _unit(name):
    name = name.lower().strip().rstrip(".")
    for table in (_VOLUME, _MASS):
        for candidate in (name, name[:-1] if name.endswith("s") else name, name[:-2] if name.endswith("es") else name):
            if candidate in table:
                return candidate, table
    if name in ("c", "celsius", "centigrade", "f", "fahrenheit"):
        return name[0], None
    raise ValueError(f"unknown unit {name!r}")


def _number(amount):
    text = f"{amount:.0f}" if abs(amount - round(amount)) < 0.05 or amount >= 20 else f"{amount:.1f}"
    return text


def convert(amount, from_unit, to_unit, ingredient=None):
    """amount from_unit in to_unit: volumes, masses, between them for a known ingredient, and
    oven temperatures. Returns {"amount", "unit", "text"}; raises ValueError when it can't."""
    amount = float(amount)
    source, source_table = _unit(from_unit)
    target, target_table = _unit(to_unit)
    if source_table is None or target_table is None:
        if source_table is not None or target_table is not None:
            raise ValueError("a temperature converts only to a temperature")
        value = amount * 9 / 5 + 32 if (source, target) == ("c", "f") else (amount - 32) * 5 / 9 if (source, target) == ("f", "c") else amount
        unit = "degrees Fahrenheit" if target == "f" else "degrees Celsius"
        return {"amount": round(value), "unit": unit, "text": f"{round(value)} {unit}"}
    base = amount * source_table[source]                      # milliliters or grams
    if source_table is not target_table:
        density = _DENSITY.get((ingredient or "").lower())
        if density is None:
            raise ValueError(f"going between volume and weight needs a known ingredient, not {ingredient!r}")
        base = base * density if source_table is _VOLUME else base / density
    value = base / target_table[target]
    unit = target + ("s" if abs(value - 1) > 0.05 and len(target) > 3 else "")
    what = f" of {ingredient}" if ingredient else ""
    return {"amount": round(value, 2), "unit": unit, "text": f"{_number(value)} {unit}{what}"}

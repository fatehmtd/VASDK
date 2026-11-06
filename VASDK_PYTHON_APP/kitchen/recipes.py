"""The recipe book, and cooking a recipe step by step."""

import json
from pathlib import Path


def spoken_list(items):
    """"a, b and c", as said."""
    items = list(items)
    if not items:
        return "nothing"
    if len(items) == 1:
        return items[0]
    return ", ".join(items[:-1]) + " and " + items[-1]


def amount_of(item, amount):
    """"200 grams of spaghetti", "a small bunch of parsley", "salt to taste": an ingredient, as said."""
    if amount[:1].isdigit():
        return f"{amount} {item}" if amount.split()[-1].isdigit() or len(amount.split()) == 1 else f"{amount} of {item}"
    if amount.split()[-1] in ("little", "few", "small", "large", "big"):
        return f"{amount} {item}"                       # "a little butter", "half a small onion"
    if amount.split()[0] in ("a", "an", "half", "some"):
        return f"{amount} of {item}"
    return f"{item} {amount}"


def _variants(phrase):
    """The words of a name, and its plural or singular: "tomato" finds "tomatoes" too."""
    words = phrase.lower().split()
    found = {tuple(words)}
    last = words[-1]
    for other in (last + "s", last + "es", last[:-1] if last.endswith("s") else None,
                  last[:-2] if last.endswith("es") else None):
        if other:
            found.add(tuple(words[:-1] + [other]))
    return found


def find_mentions(words, phrases):
    """Where each phrase is in the words, the longest first, none overlapping: what an entity
    extractor returns. phrases: {phrase: (value, spoken)}."""
    candidates = []
    for phrase, (value, spoken) in sorted(phrases.items()):
        exact = tuple(phrase.lower().split())
        for variant in _variants(phrase):
            size = len(variant)
            for start in range(len(words) - size + 1):
                if tuple(words[start:start + size]) == variant:
                    candidates.append((size, variant != exact, start, value, spoken))
    taken = set()
    found = []
    # The longest first; for the same words, the phrase they are over its plural or singular
    # ("tomatoes" is the item "tomatoes", not "tomato" + s)
    for size, _variant, start, value, spoken in sorted(candidates, key=lambda c: (-c[0], c[1], c[2], c[3])):
        span = set(range(start, start + size))
        if span & taken:
            continue
        taken |= span
        found.append({"start": start, "length": size, "value": value, "spoken": spoken})
    return sorted(found, key=lambda mention: mention["start"])


class RecipeBook:
    """The recipes in data/recipes.json, and the pantry's usual items."""

    def __init__(self, path):
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        self.recipes = {recipe["id"]: recipe for recipe in data["recipes"]}
        self.pantry = data.get("pantry", [])

    def get(self, recipe_id):
        if recipe_id not in self.recipes:
            raise KeyError(f"no recipe {recipe_id!r}")
        return self.recipes[recipe_id]

    def names(self, ids=None):
        return [self.recipes[i]["name"] for i in (ids if ids is not None else self.recipes)]

    def with_ingredient(self, item):
        item = item.lower()
        return [i for i, recipe in self.recipes.items()
                if any(item in ingredient["item"] or ingredient["item"] in item for ingredient in recipe["ingredients"])]

    def recipe_phrases(self):
        phrases = {}
        for recipe_id, recipe in self.recipes.items():
            for name in [recipe["name"]] + recipe.get("aliases", []):
                phrases[name] = (recipe_id, recipe["name"])
        return phrases

    def ingredient_phrases(self):
        items = {ingredient["item"] for recipe in self.recipes.values() for ingredient in recipe["ingredients"]}
        return {item: (item, item) for item in items | set(self.pantry)}

    def find_recipes(self, words):
        """The @recipe entity's extractor: "let's make the garlic pasta" finds aglio_olio."""
        return find_mentions(words, self.recipe_phrases())

    def find_ingredients(self, words):
        """The @ingredient entity's extractor."""
        return find_mentions(words, self.ingredient_phrases())


class Cooking:
    """Where you are in a recipe. Each call returns the state the agent reads ($session.cooking)."""

    def __init__(self, book):
        self.book = book
        self.recipe = None
        self.step = 0          # 0: not started; 1 to n: the step being done

    def start(self, recipe_id):
        self.recipe = self.book.get(recipe_id)
        self.step = 0
        return self.state()

    def next(self):
        self._needs_recipe()
        self.step = min(self.step + 1, len(self.recipe["steps"]) + 1)
        return self.state()

    def previous(self):
        self._needs_recipe()
        self.step = max(self.step - 1, 1)
        return self.state()

    def stop(self):
        self.recipe = None
        self.step = 0
        return self.state()

    def state(self):
        if self.recipe is None:
            return {"active": False}
        steps = self.recipe["steps"]
        done = self.step > len(steps)
        current = steps[self.step - 1] if 0 < self.step <= len(steps) else {}
        return {
            "active": not done,
            "recipe": self.recipe["id"],
            "name": self.recipe["name"],
            "servings": self.recipe["servings"],
            "minutes": self.recipe["minutes"],
            "step": min(self.step, len(steps)),
            "step_count": len(steps),
            "next_step": min(self.step + 1, len(steps)),
            "text": current.get("text", ""),
            "timer_minutes": current.get("timer_minutes", 0),
            "timer_label": current.get("timer_label", ""),
            "done": done,
            "ingredients_text": spoken_list(amount_of(i["item"], i["amount"]) for i in self.recipe["ingredients"]),
        }

    def _needs_recipe(self):
        if self.recipe is None:
            raise RuntimeError("no recipe is being cooked")

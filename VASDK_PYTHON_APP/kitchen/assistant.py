"""The kitchen, wired to VASDK: the agent's tools and entities, the LLM's tools, and what the
assistant knows of the kitchen ($context.kitchen, {context} in the LLM's instructions)."""

from pathlib import Path

from .pantry import ShoppingList, convert
from .recipes import Cooking, RecipeBook, spoken_list
from .timers import Timers, parse_duration

APP_DIR = Path(__file__).resolve().parent.parent


class Kitchen:
    """The recipe book, the recipe being cooked, the timers and the shopping list.

    register(registry)        the agent's call_tool tools and @recipe / @ingredient extractors
    register_llm_tools(s)     the same kitchen as tools the session's LLM may call
    attach(target)            a Session or a Conversation: what the kitchen holds reaches it, and
                              what the LLM changes reaches the agent's $session.cooking
    """

    def __init__(self, data=APP_DIR / "data" / "recipes.json", list_file=None):
        self.book = RecipeBook(data)
        self.cooking = Cooking(self.book)
        self.timers = Timers(self._timer_done)
        self.shopping = ShoppingList(list_file)
        self._target = None
        self.on_timer_done = None   # fn(label), for the app to show it

    # ---- wiring ----

    def attach(self, target):
        self._target = target
        self._publish()

    def register(self, registry):
        registry.tool("recipes", "The recipe book and the recipe being cooked")(self.recipes_tool)
        registry.tool("timers", "Kitchen timers")(self.timers_tool)
        registry.tool("shopping", "The shopping list")(self.shopping_tool)
        registry.entity_extractor("recipes")(lambda words, text: self.book.find_recipes(words))
        registry.entity_extractor("ingredients")(lambda words, text: self.book.find_ingredients(words))

    def register_llm_tools(self, session):
        """The kitchen for the LLM: what it can do when the agent hands it a turn."""
        session.tool("find_recipes", "Recipes in the book that use an ingredient, or every recipe when none is given",
                     [{"name": "ingredient", "description": "e.g. eggs", "required": False}])(self._llm_find_recipes)
        session.tool("start_recipe", "Starts cooking a recipe from the book, step by step",
                     [{"name": "recipe", "description": "its name, e.g. pancakes"}])(self._llm_start_recipe)
        session.tool("convert_units", "Converts a kitchen amount: cups, grams, ounces, tablespoons, oven temperatures",
                     [{"name": "amount", "type": "number"}, {"name": "from_unit"}, {"name": "to_unit"},
                      {"name": "ingredient", "description": "needed between volume and weight, e.g. flour", "required": False}]
                     )(lambda args: convert(args["amount"], args["from_unit"], args["to_unit"], args.get("ingredient")))
        session.tool("set_timer", "Starts a kitchen timer",
                     [{"name": "minutes", "type": "number"}, {"name": "label", "description": "what it's for, e.g. rice", "required": False}]
                     )(lambda args: self.timers_tool({"action": "start", "minutes": args["minutes"], "label": args.get("label")}))
        session.tool("add_to_shopping_list", "Adds items to the shopping list",
                     [{"name": "items", "description": "comma-separated, e.g. eggs, paper towels"}]
                     )(lambda args: self.shopping_tool({"action": "add", "item": args["items"]}))

    def context(self):
        """What the assistant knows of the kitchen right now."""
        return {"kitchen": {"cooking": self.cooking.state(), "timers": self.timers.running(),
                            "shopping_list": list(self.shopping.items), "recipes": self.book.names()}}

    # ---- the agent's tools (call_tool) ----

    def recipes_tool(self, args):
        action = args.get("action")
        if action == "list":
            names = self.book.names()
            return {"names": spoken_list(names), "count": len(names)}
        if action == "find":
            ids = self.book.with_ingredient(args.get("ingredient", ""))
            return {"names": spoken_list(self.book.names(ids)), "count": len(ids), "all": spoken_list(self.book.names())}
        handlers = {"start": lambda: self.cooking.start(args.get("recipe")), "next": self.cooking.next,
                    "previous": self.cooking.previous, "status": self.cooking.state, "stop": self.cooking.stop}
        if action not in handlers:
            raise ValueError(f"unknown recipes action {action!r}")
        state = handlers[action]()
        self._publish()
        return state

    def timers_tool(self, args):
        action = args.get("action")
        if action == "start":
            length = args.get("duration") or args.get("minutes")
            result = self.timers.start(parse_duration(length), args.get("label") or "kitchen")
        elif action == "cancel":
            result = self.timers.cancel(args.get("label"))
        elif action == "status":
            result = self.timers.status()
        else:
            raise ValueError(f"unknown timers action {action!r}")
        self._publish()
        return result

    def shopping_tool(self, args):
        action = args.get("action")
        result = {}
        if action == "add":
            items = [item.strip() for item in str(args.get("item", "")).split(",") if item.strip()]
            added = self.shopping.add(*items)
            result["item"] = spoken_list(added or items)
        elif action == "add_recipe":
            recipe = self.book.get(args.get("recipe"))
            self.shopping.add(*(ingredient["item"] for ingredient in recipe["ingredients"]))
            result["recipe"] = recipe["name"]
        elif action == "remove":
            if not self.shopping.remove(args.get("item", "")):
                result["summary"] = f"There's no {args.get('item')} on the list. {self.shopping.summary()}"
        elif action == "clear":
            self.shopping.clear()
        elif action != "read":
            raise ValueError(f"unknown shopping action {action!r}")
        result.setdefault("summary", self.shopping.summary())
        result["count"] = len(self.shopping.items)
        self._publish()
        return result

    # ---- the LLM's own ----

    def _llm_find_recipes(self, args):
        ingredient = args.get("ingredient")
        ids = self.book.with_ingredient(ingredient) if ingredient else list(self.book.recipes)
        return {"recipes": [{"name": self.book.recipes[i]["name"], "minutes": self.book.recipes[i]["minutes"]} for i in ids]}

    def _llm_start_recipe(self, args):
        found = self.book.find_recipes(str(args.get("recipe", "")).lower().split())
        if not found:
            raise ValueError(f"no recipe like {args.get('recipe')!r}; the book has {spoken_list(self.book.names())}")
        state = self.cooking.start(found[0]["value"])
        self._publish(agent_too=True)   # "next" must work: the agent's $session.cooking follows
        return state

    # ---- what changed reaches the assistant ----

    def _publish(self, agent_too=False):
        target = self._target
        if target is None:
            return
        if hasattr(target, "set_context"):   # a Session: the LLM's {context} and $context.kitchen
            target.set_context(self.context())
        if agent_too:
            target.set_param("cooking", self.cooking.state())

    def _timer_done(self, label):
        """A timer rang: the app fires the agent's "timer-done" trigger (on_timer_done)."""
        self._publish()
        if self.on_timer_done:
            self.on_timer_done(label)

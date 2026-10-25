"""Build the corpus phonon-normgen turns into golden files.

    python build_corpus.py PTTS_SRC_DIR > corpus.jsonl

PTTS_SRC_DIR is ptts/src of a gradium-ai/xn-ptts checkout at the tag the C++ port follows.

Every string literal in the `#[cfg(test)]` module of the normalizer's sources becomes a case,
inputs and expected outputs alike, in every language, so the golden file covers each test of the
reference (the reference passes its own tests, and the golden output is what it returns). Rule
modules give "word" cases for their rule, preprocess.rs and rewrite.rs give "text" cases.
Realistic assistant replies in each language come on top.
"""

import json
import pathlib
import re
import sys

LANGS = ["en", "fr", "de", "es", "pt"]
RULE_MODULES = {
    "numbers": "numbers",
    "currency": "currency",
    "dashed_digits": "dashed-digits",
    "emails": "emails",
    "urls": "urls",
    "abbreviations": "abbreviations",
    "elongations": "elongations",
    "phones": "phones",
    "times": "times",
    "dates": "dates",
    "tlds": "urls",
}
ALL_RULES = list(dict.fromkeys(RULE_MODULES.values()))


def unescape(body):
    """A Rust string literal body to the string it denotes."""
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n == "\n":  # line continuation: skip the newline and leading whitespace
            i += 2
            while i < len(body) and body[i] in " \t\n\r":
                i += 1
            continue
        if n == "u":
            end = body.index("}", i)
            out.append(chr(int(body[i + 3:end], 16)))
            i = end + 1
            continue
        out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'"}[n])
        i += 2
    return "".join(out)


def literals(path):
    src = path.read_text(encoding="utf-8")
    tests = src.split("#[cfg(test)]", 1)
    if len(tests) < 2:
        return []
    return [unescape(m) for m in re.findall(r'"((?:[^"\\]|\\.|\\\n)*)"', tests[1], re.S)]


REALISTIC = {
    "en": [
        "Your appointment is on Tuesday, March 5th at 3:30 PM.",
        "The total comes to $1,249.99, and shipping is $4.50.",
        "I found 3 flights under £350; the cheapest leaves at 07:45.",
        "It's 72°F (22°C) in Boston right now, with a 40% chance of rain.",
        "Call us at +1 650-234-9653 or email support@example.com.",
        "Visit https://www.example.com/help for more info.",
        "The population grew from 1.2 million in 1990 to 2,450,000 in 2020.",
        "Sure! Here's the plan: 1) book the hotel; 2) rent a car (~$40/day).",
        "Your card ending in -4098 was charged €12.50 on Jan. 3.",
        "The recipe needs 250 g of flour, ½ cup of sugar and 2-3 eggs.",
        "Lt. Col. Smith arrives at 0600; Sgt. Lee at 0630.",
        "That's sooo cool 😀 — rated ★★★★☆ by 12,345 users!",
        "Q3 revenue was $3.2m, up 18% YoY (vs. $2.7m).",
        "x² + y² = r², and π ≈ 3.14159.",
        "Use 1500+20 or just “about 1,520”.",
    ],
    "fr": [
        "Votre rendez-vous est le 5 mars à 14h30.",
        "Le total s'élève à 1 249,99 € et la livraison coûte 4,50 €.",
        "Il fait 22 °C à Paris, avec 40 % de risque de pluie.",
        "Appelez le 01 23 45 67 89 ou écrivez à contact@exemple.fr.",
        "La ville compte 2 150 000 habitants et un budget de 3 milliards d'euros.",
        "Rendez-vous au n°12, à 18:00 — pas avant !",
        "Visitez https://www.kyutai.fr pour en savoir plus.",
        "Prix : 0,99 € l'unité, ou 9,90 € les dix.",
        "« Bonjour », dit-il ; puis il partit (vers 9h15).",
        "Le colis pèse 1,5 kg et mesure 40 x 30 cm.",
    ],
    "de": [
        "Ihr Termin ist am 5. März um 14:30 Uhr.",
        "Der Gesamtbetrag beträgt 1.249,99 € und der Versand kostet 4,50 €.",
        "Es sind 22 °C in Berlin, bei 40 % Regenwahrscheinlichkeit.",
        "Rufen Sie +49 30 1234567 an oder schreiben Sie an info@beispiel.de.",
        "Die Stadt hat 3.500.000 Einwohner und wuchs um 2,5 %.",
        "Das dauert 8-10 Stunden; Abfahrt 07.45, Ankunft 18.20.",
        "Besuchen Sie www.beispiel.de/hilfe für Details.",
        "Er zahlte $2 Millionen für das Haus Nr. 12.",
        "„Hallo“, sagte sie — und ging.",
        "Die Formel lautet E = mc².",
    ],
    "es": [
        "Su cita es el 5 de marzo a las 14:30.",
        "El total es de 1.249,99 € y el envío cuesta 4,50 €.",
        "Hace 22 °C en Madrid, con un 40 % de probabilidad de lluvia.",
        "Llame al +34 612 345 678 o escriba a info@ejemplo.es.",
        "La ciudad tiene 3.200.000 habitantes; es el 1.er puerto del país.",
        "El n.º 5 cuesta $50 y el 3.er premio, €1000.",
        "Visite https://www.ejemplo.es para más información.",
        "¡Hola! ¿Cómo estás? Son las 9:15.",
        "Necesitas 2-3 horas y ~200 ml de agua.",
        "El precio subió un 12,5 % en 2023.",
    ],
    "pt": [
        "Sua consulta é no dia 5 de março às 14:30.",
        "O total é de R$ 1.249,99 e o frete custa R$ 4,50.",
        "Faz 22 °C em Lisboa, com 40 % de chance de chuva.",
        "Ligue para +351 912 345 678 ou escreva para contato@exemplo.pt.",
        "A empresa captou R$ 3,5 bi e vale R$1,5 mi por loja.",
        "A cidade tem 2.500.000 habitantes; casa nº 3.",
        "Visite www.exemplo.com.br para saber mais.",
        "O preço fica entre R$ 40-60 por pessoa.",
        "São 3 horas de viagem e ~150 km.",
        "Olá! Tudo bem? Até às 18h.",
    ],
}


def main():
    src = pathlib.Path(sys.argv[1])
    cases = []

    def add(kind, lang, rules, text):
        cases.append({"kind": kind, "lang": lang, "rules": rules, "text": text})

    text_sources = [src / "preprocess.rs", src / "preprocess" / "rewrite.rs"]
    for path in text_sources:
        for lit in dict.fromkeys(literals(path)):
            for lang in LANGS:
                for rules in ["default", "all", "none", "phones"]:
                    add("text", lang, rules, lit)
    # rewrite.rs tests call rewrite_word directly too.
    for lit in dict.fromkeys(literals(src / "preprocess" / "rewrite.rs")):
        for lang in LANGS:
            for rules in ["default", "all"] + ALL_RULES:
                add("word", lang, rules, lit)
    for module, rule in RULE_MODULES.items():
        for lit in dict.fromkeys(literals(src / "preprocess" / "rewrite" / f"{module}.rs")):
            for lang in LANGS:
                add("word", lang, rule, lit)
                add("word", lang, "all", lit)
                add("text", lang, "default", lit)
                add("text", lang, "all", lit)
    # prepare_text_prompt (tts_model.rs): the prompt the model is finally given.
    for lit in dict.fromkeys(literals(src / "tts_model.rs")):
        add("prompt", "en", "none", lit)
    for lang, sentences in REALISTIC.items():
        for s in sentences:
            add("prompt", lang, "none", s)
    for lang, sentences in REALISTIC.items():
        for s in sentences:
            for rules in ["default", "all", "none"]:
                add("text", lang, rules, s)
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    for c in cases:
        print(json.dumps(c, ensure_ascii=False))


if __name__ == "__main__":
    main()

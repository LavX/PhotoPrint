#!/usr/bin/env python3
"""Appends the text-slice messages to every shipped catalogue.

The source language is Hungarian, so the table below is the single place the
new strings and their ten translations live. Running the script twice is a
no-op: a message whose source is already present is left alone.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
I18N = os.path.join(ROOT, "resources", "i18n")

# source -> {locale: translation}
MESSAGES = {
    "Szöveg": {
        "en": "Text", "de": "Text", "es": "Texto", "fr": "Texte", "it": "Testo",
        "pt": "Texto", "ru": "Текст", "ja": "テキスト", "zh_CN": "文字",
        "hi": "टेक्स्ट",
    },
    "Szöveg…": {
        "en": "Text…", "de": "Text…", "es": "Texto…", "fr": "Texte…",
        "it": "Testo…", "pt": "Texto…", "ru": "Текст…", "ja": "テキスト…",
        "zh_CN": "文字…", "hi": "टेक्स्ट…",
    },
    "Szöveg hozzáadása…": {
        "en": "Add text…", "de": "Text hinzufügen…", "es": "Añadir texto…",
        "fr": "Ajouter un texte…", "it": "Aggiungi testo…",
        "pt": "Adicionar texto…", "ru": "Добавить текст…",
        "ja": "テキストを追加…", "zh_CN": "添加文字…", "hi": "टेक्स्ट जोड़ें…",
    },
    "Szöveg a lapra": {
        "en": "Text on the page", "de": "Text auf die Seite",
        "es": "Texto en la página", "fr": "Un texte sur la page",
        "it": "Testo sulla pagina", "pt": "Texto na página",
        "ru": "Текст на страницу", "ja": "ページにテキスト",
        "zh_CN": "页面上的文字", "hi": "पेज पर टेक्स्ट",
    },
    "A szöveg": {
        "en": "The text", "de": "Der Text", "es": "El texto", "fr": "Le texte",
        "it": "Il testo", "pt": "O texto", "ru": "Текст", "ja": "テキスト",
        "zh_CN": "文字", "hi": "टेक्स्ट",
    },
    "Írd be a szöveget. A sortörést az Enter adja.": {
        "en": "Type the text. Press Enter for a new line.",
        "de": "Gib den Text ein. Enter beginnt eine neue Zeile.",
        "es": "Escribe el texto. Pulsa Intro para una línea nueva.",
        "fr": "Saisis le texte. Appuie sur Entrée pour une nouvelle ligne.",
        "it": "Scrivi il testo. Premi Invio per andare a capo.",
        "pt": "Escreva o texto. Pressione Enter para uma nova linha.",
        "ru": "Введите текст. Enter начинает новую строку.",
        "ja": "テキストを入力します。改行は Enter です。",
        "zh_CN": "输入文字。按 Enter 换行。",
        "hi": "टेक्स्ट लिखें। नई पंक्ति के लिए Enter दबाएँ।",
    },
    "Betűtípus": {
        "en": "Font", "de": "Schriftart", "es": "Fuente", "fr": "Police",
        "it": "Carattere", "pt": "Fonte", "ru": "Шрифт", "ja": "フォント",
        "zh_CN": "字体", "hi": "फ़ॉन्ट",
    },
    "Betűméret": {
        "en": "Font size", "de": "Schriftgröße", "es": "Tamaño de letra",
        "fr": "Taille de police", "it": "Dimensione carattere",
        "pt": "Tamanho da fonte", "ru": "Размер шрифта", "ja": "文字サイズ",
        "zh_CN": "字号", "hi": "फ़ॉन्ट आकार",
    },
    "Félkövér": {
        "en": "Bold", "de": "Fett", "es": "Negrita", "fr": "Gras",
        "it": "Grassetto", "pt": "Negrito", "ru": "Полужирный", "ja": "太字",
        "zh_CN": "粗体", "hi": "बोल्ड",
    },
    "Dőlt": {
        "en": "Italic", "de": "Kursiv", "es": "Cursiva", "fr": "Italique",
        "it": "Corsivo", "pt": "Itálico", "ru": "Курсив", "ja": "斜体",
        "zh_CN": "斜体", "hi": "इटैलिक",
    },
    "Aláhúzott": {
        "en": "Underlined", "de": "Unterstrichen", "es": "Subrayado",
        "fr": "Souligné", "it": "Sottolineato", "pt": "Sublinhado",
        "ru": "Подчёркнутый", "ja": "下線", "zh_CN": "下划线", "hi": "रेखांकित",
    },
    "Balra igazítás": {
        "en": "Align left", "de": "Linksbündig", "es": "Alinear a la izquierda",
        "fr": "Aligner à gauche", "it": "Allinea a sinistra",
        "pt": "Alinhar à esquerda", "ru": "По левому краю", "ja": "左揃え",
        "zh_CN": "左对齐", "hi": "बाएँ संरेखित करें",
    },
    "Középre igazítás": {
        "en": "Align centre", "de": "Zentriert", "es": "Centrar",
        "fr": "Centrer", "it": "Centra", "pt": "Centralizar",
        "ru": "По центру", "ja": "中央揃え", "zh_CN": "居中对齐",
        "hi": "मध्य में संरेखित करें",
    },
    "Jobbra igazítás": {
        "en": "Align right", "de": "Rechtsbündig", "es": "Alinear a la derecha",
        "fr": "Aligner à droite", "it": "Allinea a destra",
        "pt": "Alinhar à direita", "ru": "По правому краю", "ja": "右揃え",
        "zh_CN": "右对齐", "hi": "दाएँ संरेखित करें",
    },
    "Sormagasság": {
        "en": "Line spacing", "de": "Zeilenabstand", "es": "Interlineado",
        "fr": "Interligne", "it": "Interlinea", "pt": "Espaçamento entre linhas",
        "ru": "Межстрочный интервал", "ja": "行間", "zh_CN": "行距",
        "hi": "पंक्ति अंतराल",
    },
    "Sorköz": {
        "en": "Line spacing", "de": "Zeilenabstand", "es": "Interlineado",
        "fr": "Interligne", "it": "Interlinea", "pt": "Espaçamento entre linhas",
        "ru": "Межстрочный интервал", "ja": "行間", "zh_CN": "行距",
        "hi": "पंक्ति अंतराल",
    },
    "Szöveg szerkesztése…": {
        "en": "Edit text…", "de": "Text bearbeiten…", "es": "Editar texto…",
        "fr": "Modifier le texte…", "it": "Modifica testo…",
        "pt": "Editar texto…", "ru": "Изменить текст…",
        "ja": "テキストを編集…", "zh_CN": "编辑文字…",
        "hi": "टेक्स्ट संपादित करें…",
    },
    "Írj be legalább egy látható karaktert.": {
        "en": "Type at least one visible character.",
        "de": "Gib mindestens ein sichtbares Zeichen ein.",
        "es": "Escribe al menos un carácter visible.",
        "fr": "Saisis au moins un caractère visible.",
        "it": "Scrivi almeno un carattere visibile.",
        "pt": "Escreva pelo menos um caractere visível.",
        "ru": "Введите хотя бы один видимый символ.",
        "ja": "見える文字を1文字以上入力してください。",
        "zh_CN": "请至少输入一个可见字符。",
        "hi": "कम से कम एक दिखने वाला अक्षर लिखें।",
    },
    "Ez a betűtípus ezen a gépen nincs meg, helyette a rendszer betűtípusa látszik.": {
        "en": "This font is not installed on this computer. The system font is shown instead.",
        "de": "Diese Schriftart ist auf diesem Computer nicht vorhanden. Stattdessen wird die Systemschrift angezeigt.",
        "es": "Esta fuente no está instalada en este equipo. Se muestra la fuente del sistema.",
        "fr": "Cette police n'est pas installée sur cet ordinateur. La police du système est affichée à la place.",
        "it": "Questo carattere non è installato su questo computer. Al suo posto viene mostrato il carattere di sistema.",
        "pt": "Esta fonte não está instalada neste computador. Em vez dela, aparece a fonte do sistema.",
        "ru": "Этот шрифт не установлен на этом компьютере. Вместо него показан системный шрифт.",
        "ja": "このフォントはこのコンピューターにありません。代わりにシステムのフォントで表示します。",
        "zh_CN": "这台电脑上没有这个字体，改用系统字体显示。",
        "hi": "यह फ़ॉन्ट इस कंप्यूटर पर नहीं है, इसकी जगह सिस्टम फ़ॉन्ट दिखता है।",
    },
    "A szöveg egy része lemarad a lapról.": {
        "en": "Part of the text falls off the page.",
        "de": "Ein Teil des Textes fällt von der Seite.",
        "es": "Parte del texto se sale de la página.",
        "fr": "Une partie du texte dépasse de la page.",
        "it": "Parte del testo esce dalla pagina.",
        "pt": "Parte do texto fica fora da página.",
        "ru": "Часть текста выходит за край страницы.",
        "ja": "テキストの一部がページからはみ出します。",
        "zh_CN": "部分文字超出页面。",
        "hi": "टेक्स्ट का कुछ हिस्सा पेज से बाहर चला जाता है।",
    },
    "A szöveg ki van jelölve: %1": {
        "en": "The text is selected: %1", "de": "Der Text ist ausgewählt: %1",
        "es": "El texto está seleccionado: %1",
        "fr": "Le texte est sélectionné : %1",
        "it": "Il testo è selezionato: %1",
        "pt": "O texto está selecionado: %1", "ru": "Текст выбран: %1",
        "ja": "テキストが選択されています: %1", "zh_CN": "已选中文字：%1",
        "hi": "टेक्स्ट चुना गया है: %1",
    },
    "Szöveg: %1 / %2": {
        "en": "Text: %1 of %2", "de": "Text: %1 von %2",
        "es": "Texto: %1 de %2", "fr": "Texte : %1 sur %2",
        "it": "Testo: %1 di %2", "pt": "Texto: %1 de %2",
        "ru": "Текст: %1 из %2", "ja": "テキスト: %1 / %2",
        "zh_CN": "文字：%1 / %2", "hi": "टेक्स्ट: %1 / %2",
    },
    "Szöveg mozgatása": {
        "en": "Move text", "de": "Text verschieben", "es": "Mover texto",
        "fr": "Déplacer le texte", "it": "Sposta testo", "pt": "Mover texto",
        "ru": "Переместить текст", "ja": "テキストを移動", "zh_CN": "移动文字",
        "hi": "टेक्स्ट हिलाएँ",
    },
    "Szöveg átméretezése": {
        "en": "Resize text", "de": "Text skalieren",
        "es": "Cambiar el tamaño del texto", "fr": "Redimensionner le texte",
        "it": "Ridimensiona testo", "pt": "Redimensionar texto",
        "ru": "Изменить размер текста", "ja": "テキストのサイズ変更",
        "zh_CN": "调整文字大小", "hi": "टेक्स्ट का आकार बदलें",
    },
    "Szöveg forgatása": {
        "en": "Rotate text", "de": "Text drehen", "es": "Girar texto",
        "fr": "Faire pivoter le texte", "it": "Ruota testo", "pt": "Girar texto",
        "ru": "Повернуть текст", "ja": "テキストを回転", "zh_CN": "旋转文字",
        "hi": "टेक्स्ट घुमाएँ",
    },
    "Szöveg törlése": {
        "en": "Delete text", "de": "Text löschen", "es": "Eliminar texto",
        "fr": "Supprimer le texte", "it": "Elimina testo", "pt": "Excluir texto",
        "ru": "Удалить текст", "ja": "テキストを削除", "zh_CN": "删除文字",
        "hi": "टेक्स्ट हटाएँ",
    },
    "Egy lapon legfeljebb %1 szöveg lehet.": {
        "en": "A page can hold at most %1 texts.",
        "de": "Eine Seite fasst höchstens %1 Texte.",
        "es": "Una página admite como máximo %1 textos.",
        "fr": "Une page peut contenir au maximum %1 textes.",
        "it": "Una pagina può contenere al massimo %1 testi.",
        "pt": "Uma página aceita no máximo %1 textos.",
        "ru": "На странице может быть не больше %1 текстов.",
        "ja": "1ページに置けるテキストは %1 個までです。",
        "zh_CN": "一页最多可以有 %1 个文字。",
        "hi": "एक पेज पर अधिकतम %1 टेक्स्ट रखे जा सकते हैं।",
    },
    "Még nincs matrica vagy szöveg a lapon.": {
        "en": "There is no sticker or text on the page yet.",
        "de": "Auf dieser Seite ist noch kein Aufkleber und kein Text.",
        "es": "Esta página aún no tiene pegatinas ni textos.",
        "fr": "Cette page n'a pas encore d'autocollant ni de texte.",
        "it": "Questa pagina non ha ancora adesivi o testi.",
        "pt": "Esta página ainda não tem adesivo nem texto.",
        "ru": "На этой странице пока нет наклеек и текста.",
        "ja": "このページにはまだステッカーもテキストもありません。",
        "zh_CN": "这一页还没有贴纸或文字。",
        "hi": "इस पेज पर अभी कोई स्टिकर या टेक्स्ट नहीं है।",
    },
    "Válassz egy matricát vagy szöveget a lapon.": {
        "en": "Pick a sticker or a text on the page.",
        "de": "Wähle einen Aufkleber oder Text auf der Seite.",
        "es": "Elige una pegatina o un texto de la página.",
        "fr": "Choisis un autocollant ou un texte sur la page.",
        "it": "Scegli un adesivo o un testo sulla pagina.",
        "pt": "Escolha um adesivo ou um texto na página.",
        "ru": "Выберите наклейку или текст на странице.",
        "ja": "ページ上のステッカーまたはテキストを選んでください。",
        "zh_CN": "请选择页面上的贴纸或文字。",
        "hi": "पेज पर स्टिकर या टेक्स्ट चुनें।",
    },
    "A matricák és a szövegek a fotók fölött látszanak.": {
        "en": "Stickers and texts show above the photos.",
        "de": "Aufkleber und Texte liegen über den Fotos.",
        "es": "Las pegatinas y los textos se ven sobre las fotos.",
        "fr": "Les autocollants et les textes apparaissent au-dessus des photos.",
        "it": "Adesivi e testi appaiono sopra le foto.",
        "pt": "Adesivos e textos aparecem sobre as fotos.",
        "ru": "Наклейки и текст видны поверх фотографий.",
        "ja": "ステッカーとテキストは写真の上に表示されます。",
        "zh_CN": "贴纸和文字显示在照片上方。",
        "hi": "स्टिकर और टेक्स्ट फ़ोटो के ऊपर दिखते हैं।",
    },
    "Válassz színt a szövegnek.": {
        "en": "Choose a colour for the text.",
        "de": "Wähle eine Farbe für den Text.",
        "es": "Elige un color para el texto.",
        "fr": "Choisis une couleur pour le texte.",
        "it": "Scegli un colore per il testo.",
        "pt": "Escolha uma cor para o texto.",
        "ru": "Выберите цвет текста.",
        "ja": "テキストの色を選んでください。",
        "zh_CN": "为文字选择颜色。",
        "hi": "टेक्स्ट के लिए रंग चुनें।",
    },
}


def escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def main():
    for name in sorted(os.listdir(I18N)):
        if not name.endswith(".ts"):
            continue
        locale = name[len("photoprint_"):-len(".ts")]
        path = os.path.join(I18N, name)
        with open(path, encoding="utf-8") as handle:
            xml = handle.read()
        additions = []
        for source, translations in MESSAGES.items():
            if "<source>%s</source>" % escape(source) in xml:
                continue
            translation = translations[locale]
            additions.append(
                "    <message>\n"
                "        <source>%s</source>\n"
                "        <translation>%s</translation>\n"
                "    </message>\n" % (escape(source), escape(translation)))
        if not additions:
            continue
        marker = "</context>"
        index = xml.rindex(marker)
        xml = xml[:index] + "".join(additions) + xml[index:]
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(xml)
        print("updated", name, len(additions))


if __name__ == "__main__":
    main()

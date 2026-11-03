#!/usr/bin/env python3
"""Appends the sticker-slice messages to every shipped catalogue.

The source language is Hungarian, so the table below is the single place the
new strings and their ten translations live. Running the script twice is a
no-op: a message whose source is already present is left alone.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
I18N = os.path.join(ROOT, "resources", "i18n")

# source -> {locale: translation}
MESSAGES = {
    "A matrica képe megváltozott.": {
        "en": "The sticker's picture has changed.",
        "de": "Das Bild des Aufklebers hat sich geändert.",
        "es": "La imagen de la pegatina ha cambiado.",
        "fr": "L'image de l'autocollant a changé.",
        "it": "L'immagine dell'adesivo è cambiata.",
        "pt": "A imagem do adesivo mudou.",
        "ru": "Изображение наклейки изменилось.",
        "ja": "ステッカーの画像が変更されています。",
        "zh_CN": "贴纸的图片已更改。",
        "hi": "स्टिकर की तस्वीर बदल गई है।",
    },
	"Képek (*.png *.jpg *.jpeg *.webp)": {
        "en": "Pictures (*.png *.jpg *.jpeg *.webp)",
        "de": "Bilder (*.png *.jpg *.jpeg *.webp)",
        "es": "Imágenes (*.png *.jpg *.jpeg *.webp)",
        "fr": "Images (*.png *.jpg *.jpeg *.webp)",
        "it": "Immagini (*.png *.jpg *.jpeg *.webp)",
        "pt": "Imagens (*.png *.jpg *.jpeg *.webp)",
        "ru": "Изображения (*.png *.jpg *.jpeg *.webp)",
        "ja": "画像 (*.png *.jpg *.jpeg *.webp)",
        "zh_CN": "图片 (*.png *.jpg *.jpeg *.webp)",
        "hi": "चित्र (*.png *.jpg *.jpeg *.webp)",
    },
    "Egy lépéssel előrébb.": {
        "en": "One step forward.",
        "de": "Einen Schritt nach vorne.",
        "es": "Un paso adelante.",
        "fr": "Un pas en avant.",
        "it": "Un passo avanti.",
        "pt": "Um passo à frente.",
        "ru": "На один шаг вперёд.",
        "ja": "1 段階前へ。",
        "zh_CN": "向前一层。",
        "hi": "एक कदम आगे।",
    },
    "Egy lépéssel hátrébb.": {
        "en": "One step backward.",
        "de": "Einen Schritt nach hinten.",
        "es": "Un paso atrás.",
        "fr": "Un pas en arrière.",
        "it": "Un passo indietro.",
        "pt": "Um passo atrás.",
        "ru": "На один шаг назад.",
        "ja": "1 段階後ろへ。",
        "zh_CN": "向后一层。",
        "hi": "एक कदम पीछे।",
    },
    "Matrica: %1 / %2": {
        "en": "Sticker: %1 of %2",
        "de": "Aufkleber: %1 von %2",
        "es": "Pegatina: %1 de %2",
        "fr": "Autocollant : %1 sur %2",
        "it": "Adesivo: %1 di %2",
        "pt": "Adesivo: %1 de %2",
        "ru": "Наклейка: %1 из %2",
        "ja": "ステッカー: %1 / %2",
        "zh_CN": "贴纸：%1 / %2",
        "hi": "स्टिकर: %1 / %2",
    },
    "Matrica hozzáadása": {
        "en": "Add sticker",
        "de": "Aufkleber hinzufügen",
        "es": "Añadir pegatina",
        "fr": "Ajouter un autocollant",
        "it": "Aggiungi adesivo",
        "pt": "Adicionar adesivo",
        "ru": "Добавить наклейку",
        "ja": "ステッカーを追加",
        "zh_CN": "添加贴纸",
        "hi": "स्टिकर जोड़ें",
    },
    "Matrica fedettsége": {
        "en": "Sticker opacity",
        "de": "Aufkleber-Deckkraft",
        "es": "Opacidad de la pegatina",
        "fr": "Opacité de l'autocollant",
        "it": "Opacità dell'adesivo",
        "pt": "Opacidade do adesivo",
        "ru": "Прозрачность наклейки",
        "ja": "ステッカーの不透明度",
        "zh_CN": "贴纸不透明度",
        "hi": "स्टिकर की अपारदर्शिता",
    },
    "A matrica ki van jelölve.": {
        "en": "The sticker is selected.",
        "de": "Der Aufkleber ist ausgewählt.",
        "es": "La pegatina está seleccionada.",
        "fr": "L'autocollant est sélectionné.",
        "it": "L'adesivo è selezionato.",
        "pt": "O adesivo está selecionado.",
        "ru": "Наклейка выбрана.",
        "ja": "ステッカーが選択されています。",
        "zh_CN": "已选中贴纸。",
        "hi": "स्टिकर चुना गया है।",
    },
    "Kiválasztott elem eltávolítása": {
        "en": "Remove selected element",
        "de": "Ausgewähltes Element entfernen",
        "es": "Quitar el elemento seleccionado",
        "fr": "Supprimer l'élément sélectionné",
        "it": "Rimuovi l'elemento selezionato",
        "pt": "Remover o elemento selecionado",
        "ru": "Удалить выбранный элемент",
        "ja": "選択した要素を削除",
        "zh_CN": "移除所选元素",
        "hi": "चुना हुआ तत्व हटाएँ",
    },
    "Elem": {
        "en": "Element", "de": "Element", "es": "Elemento", "fr": "Élément",
        "it": "Elemento", "pt": "Elemento", "ru": "Элемент", "ja": "要素",
        "zh_CN": "元素", "hi": "तत्व",
    },
    "Beszúrás": {
        "en": "Insert", "de": "Einfügen", "es": "Insertar", "fr": "Insérer",
        "it": "Inserisci", "pt": "Inserir", "ru": "Вставка", "ja": "挿入",
        "zh_CN": "插入", "hi": "सम्मिलित करें",
    },
    "Matrica…": {
        "en": "Sticker…", "de": "Aufkleber…", "es": "Pegatina…",
        "fr": "Autocollant…", "it": "Adesivo…", "pt": "Adesivo…",
        "ru": "Наклейка…", "ja": "ステッカー…", "zh_CN": "贴纸…",
        "hi": "स्टिकर…",
    },
    "Matrica hozzáadása…": {
        "en": "Add sticker…", "de": "Aufkleber hinzufügen…",
        "es": "Añadir pegatina…", "fr": "Ajouter un autocollant…",
        "it": "Aggiungi adesivo…", "pt": "Adicionar adesivo…",
        "ru": "Добавить наклейку…", "ja": "ステッカーを追加…",
        "zh_CN": "添加贴纸…", "hi": "स्टिकर जोड़ें…",
    },
    "Készlet": {
        "en": "Set", "de": "Auswahl", "es": "Colección", "fr": "Collection",
        "it": "Collezione", "pt": "Coleção", "ru": "Набор", "ja": "セット",
        "zh_CN": "套装", "hi": "सेट",
    },
    "Matricakészlet": {
        "en": "Sticker set", "de": "Aufkleber-Auswahl",
        "es": "Colección de pegatinas", "fr": "Collection d'autocollants",
        "it": "Collezione di adesivi", "pt": "Coleção de adesivos",
        "ru": "Набор наклеек", "ja": "ステッカーセット", "zh_CN": "贴纸套装",
        "hi": "स्टिकर सेट",
    },
    "Csillag": {
        "en": "Star", "de": "Stern", "es": "Estrella", "fr": "Étoile",
        "it": "Stella", "pt": "Estrela", "ru": "Звезда", "ja": "星",
        "zh_CN": "星星", "hi": "तारा",
    },
    "Szív": {
        "en": "Heart", "de": "Herz", "es": "Corazón", "fr": "Cœur",
        "it": "Cuore", "pt": "Coração", "ru": "Сердце", "ja": "ハート",
        "zh_CN": "爱心", "hi": "दिल",
    },
    "Nyíl": {
        "en": "Arrow", "de": "Pfeil", "es": "Flecha", "fr": "Flèche",
        "it": "Freccia", "pt": "Seta", "ru": "Стрелка", "ja": "矢印",
        "zh_CN": "箭头", "hi": "तीर",
    },
    "Beszédbuborék": {
        "en": "Speech bubble", "de": "Sprechblase", "es": "Bocadillo",
        "fr": "Bulle", "it": "Fumetto", "pt": "Balão de fala",
        "ru": "Облачко", "ja": "吹き出し", "zh_CN": "对话气泡",
        "hi": "संवाद गुब्बारा",
    },
    "Nap": {
        "en": "Sun", "de": "Sonne", "es": "Sol", "fr": "Soleil", "it": "Sole",
        "pt": "Sol", "ru": "Солнце", "ja": "太陽", "zh_CN": "太阳",
        "hi": "सूरज",
    },
    "Virág": {
        "en": "Flower", "de": "Blume", "es": "Flor", "fr": "Fleur",
        "it": "Fiore", "pt": "Flor", "ru": "Цветок", "ja": "花",
        "zh_CN": "花朵", "hi": "फूल",
    },
    "Konfetti": {
        "en": "Confetti", "de": "Konfetti", "es": "Confeti", "fr": "Confettis",
        "it": "Coriandoli", "pt": "Confete", "ru": "Конфетти", "ja": "紙吹雪",
        "zh_CN": "彩纸", "hi": "कॉन्फ़ेटी",
    },
    "Keret": {
        "en": "Frame", "de": "Rahmen", "es": "Marco", "fr": "Cadre",
        "it": "Cornice", "pt": "Moldura", "ru": "Рамка", "ja": "フレーム",
        "zh_CN": "边框", "hi": "फ़्रेम",
    },
    "Kör": {
        "en": "Circle", "de": "Kreis", "es": "Círculo", "fr": "Cercle",
        "it": "Cerchio", "pt": "Círculo", "ru": "Круг", "ja": "円",
        "zh_CN": "圆形", "hi": "वृत्त",
    },
    "Szikra": {
        "en": "Sparkle", "de": "Funkeln", "es": "Destello", "fr": "Étincelle",
        "it": "Scintilla", "pt": "Brilho", "ru": "Искра", "ja": "きらめき",
        "zh_CN": "闪光", "hi": "चमक",
    },
    "Szalag": {
        "en": "Ribbon", "de": "Band", "es": "Cinta", "fr": "Ruban",
        "it": "Nastro", "pt": "Fita", "ru": "Лента", "ja": "リボン",
        "zh_CN": "丝带", "hi": "रिबन",
    },
    "Jelvény": {
        "en": "Badge", "de": "Abzeichen", "es": "Insignia", "fr": "Écusson",
        "it": "Distintivo", "pt": "Distintivo", "ru": "Значок", "ja": "バッジ",
        "zh_CN": "徽章", "hi": "बैज",
    },
    "Matrica": {
        "en": "Sticker", "de": "Aufkleber", "es": "Pegatina",
        "fr": "Autocollant", "it": "Adesivo", "pt": "Adesivo",
        "ru": "Наклейка", "ja": "ステッカー", "zh_CN": "贴纸", "hi": "स्टिकर",
    },
    "Elforgatás": {
        "en": "Rotation", "de": "Drehung", "es": "Rotación", "fr": "Rotation",
        "it": "Rotazione", "pt": "Rotação", "ru": "Поворот", "ja": "回転",
        "zh_CN": "旋转", "hi": "घुमाव",
    },
    "Előrébb": {
        "en": "Bring forward", "de": "Nach vorne", "es": "Traer adelante",
        "fr": "Avancer", "it": "Porta avanti", "pt": "Trazer para frente",
        "ru": "Вперёд", "ja": "前面へ", "zh_CN": "上移一层", "hi": "आगे लाएँ",
    },
    "Hátrébb": {
        "en": "Send backward", "de": "Nach hinten", "es": "Enviar atrás",
        "fr": "Reculer", "it": "Porta indietro", "pt": "Enviar para trás",
        "ru": "Назад", "ja": "背面へ", "zh_CN": "下移一层", "hi": "पीछे भेजें",
    },
    "Ezt a matricát nem találjuk.": {
        "en": "This sticker cannot be found.",
        "de": "Dieser Aufkleber wurde nicht gefunden.",
        "es": "No se encuentra esta pegatina.",
        "fr": "Cet autocollant est introuvable.",
        "it": "Impossibile trovare questo adesivo.",
        "pt": "Não encontramos este adesivo.",
        "ru": "Эта наклейка не найдена.",
        "ja": "このステッカーが見つかりません。",
        "zh_CN": "找不到这个贴纸。",
        "hi": "यह स्टिकर नहीं मिल रहा.",
    },
    "Matrica kiválasztása": {
        "en": "Choose sticker", "de": "Aufkleber auswählen",
        "es": "Elegir pegatina", "fr": "Choisir un autocollant",
        "it": "Scegli adesivo", "pt": "Escolher adesivo",
        "ru": "Выбрать наклейку", "ja": "ステッカーを選ぶ",
        "zh_CN": "选择贴纸", "hi": "स्टिकर चुनें",
    },
    "Előző elem": {
        "en": "Previous element", "de": "Vorheriges Element",
        "es": "Elemento anterior", "fr": "Élément précédent",
        "it": "Elemento precedente", "pt": "Elemento anterior",
        "ru": "Предыдущий элемент", "ja": "前の要素", "zh_CN": "上一个元素",
        "hi": "पिछला तत्व",
    },
    "Következő elem": {
        "en": "Next element", "de": "Nächstes Element",
        "es": "Elemento siguiente", "fr": "Élément suivant",
        "it": "Elemento successivo", "pt": "Próximo elemento",
        "ru": "Следующий элемент", "ja": "次の要素", "zh_CN": "下一个元素",
        "hi": "अगला तत्व",
    },
    "A matrica a fotók fölött látszik.": {
        "en": "The sticker shows above the photos.",
        "de": "Der Aufkleber liegt über den Fotos.",
        "es": "La pegatina se ve sobre las fotos.",
        "fr": "L'autocollant apparaît au-dessus des photos.",
        "it": "L'adesivo appare sopra le foto.",
        "pt": "O adesivo aparece sobre as fotos.",
        "ru": "Наклейка видна поверх фотографий.",
        "ja": "ステッカーは写真の上に表示されます。",
        "zh_CN": "贴纸显示在照片上方。",
        "hi": "स्टिकर फ़ोटो के ऊपर दिखता है।",
    },
    "Még nincs matrica a lapon.": {
        "en": "There is no sticker on the page yet.",
        "de": "Auf dieser Seite ist noch kein Aufkleber.",
        "es": "Esta página aún no tiene pegatinas.",
        "fr": "Cette page n'a pas encore d'autocollant.",
        "it": "Questa pagina non ha ancora adesivi.",
        "pt": "Esta página ainda não tem adesivo.",
        "ru": "На этой странице пока нет наклеек.",
        "ja": "このページにはまだステッカーがありません。",
        "zh_CN": "这一页还没有贴纸。",
        "hi": "इस पेज पर अभी कोई स्टिकर नहीं है।",
    },
    "Válassz egy matricát a lapon vagy a készletből.": {
        "en": "Pick a sticker on the page or from the set.",
        "de": "Wähle einen Aufkleber auf der Seite oder aus der Auswahl.",
        "es": "Elige una pegatina de la página o de la colección.",
        "fr": "Choisis un autocollant sur la page ou dans la collection.",
        "it": "Scegli un adesivo sulla pagina o dalla collezione.",
        "pt": "Escolha um adesivo na página ou na coleção.",
        "ru": "Выберите наклейку на странице или в наборе.",
        "ja": "ページ上またはセットからステッカーを選んでください。",
        "zh_CN": "请选择页面上的贴纸或套装中的贴纸。",
        "hi": "पेज पर या सेट से स्टिकर चुनें।",
    },
    "Matrica mozgatása": {
        "en": "Move sticker", "de": "Aufkleber verschieben",
        "es": "Mover pegatina", "fr": "Déplacer l'autocollant",
        "it": "Sposta adesivo", "pt": "Mover adesivo",
        "ru": "Переместить наклейку", "ja": "ステッカーを移動",
        "zh_CN": "移动贴纸", "hi": "स्टिकर हिलाएँ",
    },
    "Matrica átméretezése": {
        "en": "Resize sticker", "de": "Aufkleber skalieren",
        "es": "Cambiar el tamaño de la pegatina",
        "fr": "Redimensionner l'autocollant", "it": "Ridimensiona adesivo",
        "pt": "Redimensionar adesivo", "ru": "Изменить размер наклейки",
        "ja": "ステッカーのサイズ変更", "zh_CN": "调整贴纸大小",
        "hi": "स्टिकर का आकार बदलें",
    },
    "Matrica forgatása": {
        "en": "Rotate sticker", "de": "Aufkleber drehen",
        "es": "Girar pegatina", "fr": "Faire pivoter l'autocollant",
        "it": "Ruota adesivo", "pt": "Girar adesivo",
        "ru": "Повернуть наклейку", "ja": "ステッカーを回転",
        "zh_CN": "旋转贴纸", "hi": "स्टिकर घुमाएँ",
    },
    "Matrica törlése": {
        "en": "Delete sticker", "de": "Aufkleber löschen",
        "es": "Eliminar pegatina", "fr": "Supprimer l'autocollant",
        "it": "Elimina adesivo", "pt": "Excluir adesivo",
        "ru": "Удалить наклейку", "ja": "ステッカーを削除",
        "zh_CN": "删除贴纸", "hi": "स्टिकर हटाएँ",
    },
    "Képek (*.png *.jpg *.jpeg *.webp *.gif)": {
        "en": "Pictures (*.png *.jpg *.jpeg *.webp *.gif)",
        "de": "Bilder (*.png *.jpg *.jpeg *.webp *.gif)",
        "es": "Imágenes (*.png *.jpg *.jpeg *.webp *.gif)",
        "fr": "Images (*.png *.jpg *.jpeg *.webp *.gif)",
        "it": "Immagini (*.png *.jpg *.jpeg *.webp *.gif)",
        "pt": "Imagens (*.png *.jpg *.jpeg *.webp *.gif)",
        "ru": "Изображения (*.png *.jpg *.jpeg *.webp *.gif)",
        "ja": "画像 (*.png *.jpg *.jpeg *.webp *.gif)",
        "zh_CN": "图片 (*.png *.jpg *.jpeg *.webp *.gif)",
        "hi": "चित्र (*.png *.jpg *.jpeg *.webp *.gif)",
    },
    "Egy lapon legfeljebb %1 matrica lehet.": {
        "en": "A page can hold at most %1 stickers.",
        "de": "Eine Seite fasst höchstens %1 Aufkleber.",
        "es": "Una página admite como máximo %1 pegatinas.",
        "fr": "Une page peut contenir au maximum %1 autocollants.",
        "it": "Una pagina può contenere al massimo %1 adesivi.",
        "pt": "Uma página aceita no máximo %1 adesivos.",
        "ru": "На странице может быть не больше %1 наклеек.",
        "ja": "1ページに置けるステッカーは %1 個までです。",
        "zh_CN": "一页最多可以有 %1 个贴纸。",
        "hi": "एक पेज पर अधिकतम %1 स्टिकर रखे जा सकते हैं।",
    },
    "A matricát nem sikerült a helyére tenni.": {
        "en": "The sticker could not be placed.",
        "de": "Der Aufkleber konnte nicht abgelegt werden.",
        "es": "No se pudo colocar la pegatina.",
        "fr": "L'autocollant n'a pas pu être placé.",
        "it": "Impossibile inserire l'adesivo.",
        "pt": "Não foi possível colocar o adesivo.",
        "ru": "Не удалось разместить наклейку.",
        "ja": "ステッカーを配置できませんでした。",
        "zh_CN": "无法放置贴纸。",
        "hi": "स्टिकर नहीं रखा जा सका।",
    },
    "A matrica túl nagy vagy nem olvasható.": {
        "en": "The sticker is too large or cannot be read.",
        "de": "Der Aufkleber ist zu groß oder nicht lesbar.",
        "es": "La pegatina es demasiado grande o no se puede leer.",
        "fr": "L'autocollant est trop volumineux ou illisible.",
        "it": "L'adesivo è troppo grande o illeggibile.",
        "pt": "O adesivo é grande demais ou não pode ser lido.",
        "ru": "Наклейка слишком большая или не читается.",
        "ja": "ステッカーが大きすぎるか、読み取れません。",
        "zh_CN": "贴纸太大或无法读取。",
        "hi": "स्टिकर बहुत बड़ा है या पढ़ा नहीं जा सकता।",
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

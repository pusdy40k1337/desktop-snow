#include "ui_strings.h"

#include <cstddef>
#include <vector>

namespace snow {

namespace {
struct Entry {
  const char* en;
  const char* ru;
  const char* uk;
};

// Order must match enum class UiStr.
const Entry kTable[] = {
    // Tray, notifications and live status.
    {"Settings…", "Настройки…", "Налаштування…"},
    {"Pause for 1 hour", "Пауза на 1 час", "Пауза на 1 годину"},
    {"Resume snow", "Возобновить снег", "Відновити сніг"},
    {"Diagnostics copied to the clipboard.", "Диагностика скопирована в буфер обмена.",
     "Діагностику скопійовано в буфер обміну."},
    {"Diagnostics saved to:", "Диагностика сохранена в файл:", "Діагностику збережено у файл:"},
    {"Could not save the diagnostics file.", "Не удалось сохранить файл диагностики.",
     "Не вдалося зберегти файл діагностики."},
    {"Monitor", "Монитор", "Монітор"},
    {"snowing", "идёт снег", "іде сніг"},
    {"paused: fullscreen app", "пауза: полноэкранное приложение", "пауза: повноекранна програма"},
    {"paused: maximized window", "пауза: развёрнутое окно", "пауза: розгорнуте вікно"},
    {"snow switched off for this monitor", "снег выключен на этом мониторе", "сніг вимкнено на цьому моніторі"},
    {"snow is switched off", "снег выключен", "сніг вимкнено"},
    {"paused for 1 hour", "пауза на 1 час", "пауза на 1 годину"},
    {"paused: screen locked or asleep", "пауза: экран заблокирован или спит", "пауза: екран заблоковано або він спить"},
    {"paused: on battery", "пауза: работа от батареи", "пауза: живлення від батареї"},
    {"Reset all settings to their defaults?", "Сбросить все настройки до значений по умолчанию?",
     "Скинути всі налаштування до типових значень?"},
    {"Pause when a window is maximized", "Пауза, когда окно развёрнуто", "Пауза, коли вікно розгорнуто"},
    // Window and sections.
    {"Settings", "Настройки", "Налаштування"},
    {"General", "Общие", "Загальні"},
    {"Snow", "Снег", "Сніг"},
    {"Decorations", "Украшения", "Прикраси"},
    {"Monitors", "Мониторы", "Монітори"},
    {"Performance", "Производительность", "Продуктивність"},
    {"Diagnostics", "Диагностика", "Діагностика"},
    {"About", "О программе", "Про програму"},
    {"On", "Вкл.", "Увімк."},
    {"Off", "Выкл.", "Вимк."},
    {"Auto", "Авто", "Авто"},
    {"px", "пикс.", "пікс."},
    {"px/s", "пикс./с", "пікс./с"},
    // General.
    {"Snowfall", "Снегопад", "Снігопад"},
    {"Turn all snow on or off", "Включить или выключить весь снег", "Увімкнути або вимкнути весь сніг"},
    {"Launch at Windows startup", "Запускать вместе с Windows", "Запускати разом із Windows"},
    {"Starts automatically when you sign in", "Запуск при входе в систему", "Запуск під час входу в систему"},
    {"Language", "Язык", "Мова"},
    {"Auto follows the Windows language", "«Авто» — как в Windows", "«Авто» — як у Windows"},
    {"Shortcut: snow on/off", "Сочетание клавиш: снег вкл./выкл.", "Сполучення клавіш: сніг увімк./вимк."},
    {"Shortcut: decorate", "Сочетание клавиш: украсить", "Сполучення клавіш: прикрасити"},
    {"Click, then press the new key combination", "Нажмите, затем введите новое сочетание клавиш",
     "Натисніть, потім введіть нове сполучення клавіш"},
    {"Press a new shortcut…", "Нажмите сочетание…", "Натисніть сполучення…"},
    {"Ctrl, Alt, Shift or Win together with a letter, digit or F-key. Esc cancels.",
     "Ctrl, Alt, Shift или Win вместе с буквой, цифрой или F-клавишей. Esc — отмена.",
     "Ctrl, Alt, Shift або Win разом із літерою, цифрою чи F-клавішею. Esc — скасувати."},
    {"Pause during fullscreen", "Пауза в полноэкранном режиме", "Пауза в повноекранному режимі"},
    {"Games, fullscreen video, F11 browsers and slideshows",
     "Игры, видео на весь экран, браузер в режиме F11 и презентации",
     "Ігри, відео на весь екран, браузер у режимі F11 і презентації"},
    {"No snow on a monitor while a maximized window is on top",
     "Не показывать снег на мониторе, пока сверху развёрнутое окно",
     "Не показувати сніг на моніторі, поки згори розгорнуте вікно"},
    {"On battery", "От батареи", "Від батареї"},
    {"What to do when the PC runs on battery", "Что делать, когда компьютер работает от батареи",
     "Що робити, коли комп'ютер працює від батареї"},
    {"Full snow", "Полный снег", "Повний сніг"},
    {"Save battery", "Экономить", "Економити"},
    {"Pause snow", "Пауза", "Пауза"},
    // Snow.
    {"What falls", "Что падает", "Що падає"},
    {"Snow", "Снег", "Сніг"},
    {"Confetti", "Конфетти", "Конфеті"},
    {"Nothing", "Ничего", "Нічого"},
    {"Intensity", "Интенсивность", "Інтенсивність"},
    {"How much snow falls at once", "Сколько снега падает одновременно", "Скільки снігу падає одночасно"},
    {"Flake size", "Размер снежинок", "Розмір сніжинок"},
    {"Fall speed", "Скорость падения", "Швидкість падіння"},
    {"Wind", "Ветер", "Вітер"},
    {"Left or right; 0 is calm", "Влево или вправо; 0 — штиль", "Ліворуч або праворуч; 0 — штиль"},
    {"Wind gusts", "Порывы ветра", "Пориви вітру"},
    {"Snow color", "Цвет снега", "Колір снігу"},
    {"White", "Белый", "Білий"},
    {"Ice blue", "Ледяной", "Крижаний"},
    {"Warm", "Тёплый", "Теплий"},
    {"Pink", "Розовый", "Рожевий"},
    {"Sparkle and glow", "Искры и сияние", "Іскри та сяйво"},
    {"Flakes twinkle softly", "Снежинки мягко мерцают", "Сніжинки м'яко мерехтять"},
    {"Snow piles on windows", "Сугробы на окнах", "Кучугури на вікнах"},
    {"Snow settles on the top edge of windows", "Снег ложится на верхний край окон",
     "Сніг лягає на верхній край вікон"},
    {"Pile height", "Высота сугробов", "Висота кучугур"},
    {"Snow on the taskbar", "Снег на панели задач", "Сніг на панелі завдань"},
    {"The snow line along the top of the taskbar stays behind windows",
     "Полоса снега над панелью задач остаётся позади окон",
     "Смуга снігу над панеллю завдань залишається позаду вікон"},
    {"Shake-off speed", "Скорость стряхивания", "Швидкість струшування"},
    {"How fast a window must move to shake its snow off", "Как быстро нужно двигать окно, чтобы стряхнуть снег",
     "Як швидко треба рухати вікно, щоб струсити сніг"},
    {"Cursor sweeps snow", "Курсор сметает снег", "Курсор змітає сніг"},
    {"Slow melting", "Медленное таяние", "Повільне танення"},
    {"Piles slowly melt away", "Сугробы понемногу тают", "Кучугури потроху тануть"},
    {"Day/night tint", "Оттенок дня и ночи", "Відтінок дня і ночі"},
    {"Snow looks a little bluer at night", "Ночью снег чуть голубее", "Уночі сніг трохи блакитніший"},
    {"Reduced motion", "Меньше движения", "Менше руху"},
    {"Auto follows the Windows animation setting", "«Авто» — как настройка анимации Windows",
     "«Авто» — як налаштування анімації Windows"},
    // Decorations.
    {"Decorate now", "Украсить сейчас", "Прикрасити зараз"},
    {"Place trees, ornaments and snowmen with the mouse. Shortcut: {0}",
     "Расставьте ёлки, игрушки и снеговиков мышью. Сочетание клавиш: {0}",
     "Розставте ялинки, іграшки та сніговиків мишею. Сполучення клавіш: {0}"},
    {"Decorate", "Украсить", "Прикрасити"},
    {"On your desktop", "На рабочем столе", "На робочому столі"},
    {"Trees: {0} of {1}, snowmen: {2} of {3}", "Ёлки: {0} из {1}, снеговики: {2} из {3}",
     "Ялинки: {0} з {1}, сніговики: {2} з {3}"},
    {"Decoration theme", "Тема украшений", "Тема прикрас"},
    {"Classic", "Классика", "Класика"},
    {"Frost", "Иней", "Іній"},
    {"Candy", "Леденцы", "Льодяники"},
    {"Golden night", "Золотая ночь", "Золота ніч"},
    {"Santa visits", "Визиты Деда Мороза", "Візити Святого Миколая"},
    {"Santa peeks in from the edge of the screen now and then", "Дед Мороз иногда заглядывает с края экрана",
     "Святий Миколай час від часу зазирає з краю екрана"},
    {"Gift train", "Поезд с подарками", "Потяг із подарунками"},
    {"A little train with gifts drives by now and then", "Иногда проезжает поезд с подарками",
     "Час від часу проїжджає потяг із подарунками"},
    {"The train knocks snowmen down", "Поезд сбивает снеговиков", "Потяг збиває сніговиків"},
    {"Snowmen on the rails get knocked flying when the train passes (they come back a moment later)",
     "Снеговики на рельсах разлетаются от удара поезда и через пару секунд встают на место",
     "Сніговики на рейках розлітаються від удару потяга і за кілька секунд повертаються на місце"},
    {"Make snowballs", "Лепить снежки", "Ліпити сніжки"},
    {"Hold the left mouse button on a snow drift and roll a ball along it: the drift shrinks, the ball grows",
     "Зажмите ЛКМ на сугробе и катите ком вдоль него: сугроб убывает, ком растёт",
     "Затисніть ЛКМ на заметі та котіть ком вздовж нього: замет зменшується, ком росте"},
    {"Yeti", "Йети", "Єті"},
    {"A yeti sometimes hunts a snowman", "Йети иногда охотится на снеговика", "Єті іноді полює на сніговика"},
    {"Call a visitor now", "Позвать гостя сейчас", "Покликати гостя зараз"},
    {"Santa", "Дед Мороз", "Миколай"},
    {"Train", "Поезд", "Потяг"},
    {"Yeti", "Йети", "Єті"},
    {"Remove all decorations", "Убрать все украшения", "Прибрати всі прикраси"},
    {"Trees and snowmen disappear", "Ёлки и снеговики исчезнут", "Ялинки та сніговики зникнуть"},
    {"Clear snow", "Убрать снег", "Прибрати сніг"},
    {"Removes the snow piles from windows and the taskbar", "Убирает сугробы с окон и панели задач",
     "Прибирає кучугури з вікон і панелі завдань"},
    {"Clear", "Убрать", "Прибрати"},
    // Monitors.
    {"Snow on each monitor", "Снег на каждом мониторе", "Сніг на кожному моніторі"},
    {"Switch a monitor off here to keep it free of snow", "Выключите монитор здесь, чтобы на нём не было снега",
     "Вимкніть монітор тут, щоб на ньому не було снігу"},
    {"Monitor {0}", "Монитор {0}", "Монітор {0}"},
    {"main", "основной", "основний"},
    {"No monitors found", "Мониторы не найдены", "Монітори не знайдено"},
    // Performance.
    {"Frame rate limit", "Ограничение частоты кадров", "Обмеження частоти кадрів"},
    {"Lower values use less CPU and GPU", "Меньше — ниже нагрузка на процессор и видеокарту",
     "Менше — нижче навантаження на процесор і відеокарту"},
    {"Current frame rate", "Текущая частота кадров", "Поточна частота кадрів"},
    {"{0} fps", "{0} кадр/с", "{0} кадр/с"},
    {"Battery saving", "Экономия батареи", "Економія батареї"},
    {"With “Save battery” chosen, on battery or in battery saver the snow is halved and capped at 30 fps",
     "В режиме «Экономить» при работе от батареи или в режиме экономии снега вдвое меньше, не более 30 кадр/с",
     "У режимі «Економити» під час роботи від батареї або в режимі економії снігу вдвічі менше, не більше 30 кадр/с"},
    // Diagnostics.
    {"Debug layer", "Отладочный слой", "Налагоджувальний шар"},
    {"Fps, sync mode and pause decisions on screen", "Кадры/с, режим синхронизации и решения о паузе на экране",
     "Кадри/с, режим синхронізації та рішення про паузу на екрані"},
    {"Log pause decisions every 0.5 s", "Записывать решения о паузе каждые 0,5 с",
     "Записувати рішення про паузу кожні 0,5 с"},
    {"Writes to DesktopSnow-log.txt until the app closes", "Пишет в DesktopSnow-log.txt до закрытия программы",
     "Пише в DesktopSnow-log.txt до закриття програми"},
    {"Pause decision now", "Решение о паузе сейчас", "Рішення про паузу зараз"},
    {"Updated every 0.5 s", "Обновляется каждые 0,5 с", "Оновлюється кожні 0,5 с"},
    {"No data yet", "Пока нет данных", "Поки немає даних"},
    {"Copy diagnostics", "Скопировать диагностику", "Скопіювати діагностику"},
    {"Copies a report to the clipboard to paste into a message",
     "Копирует отчёт в буфер обмена, чтобы вставить в сообщение",
     "Копіює звіт у буфер обміну, щоб вставити в повідомлення"},
    {"Copy", "Копировать", "Копіювати"},
    {"Save diagnostics to a file", "Сохранить диагностику в файл", "Зберегти діагностику у файл"},
    {"Saves the same report as a text file", "Сохраняет тот же отчёт в текстовый файл",
     "Зберігає той самий звіт у текстовий файл"},
    {"Save", "Сохранить", "Зберегти"},
    {"Open settings folder", "Открыть папку настроек", "Відкрити теку налаштувань"},
    {"Open log folder", "Открыть папку журналов", "Відкрити теку журналів"},
    {"Open", "Открыть", "Відкрити"},
    // About.
    {"Build: {0}", "Сборка: {0}", "Збірка: {0}"},
    {"Snow falls over your desktop and settles on windows and the taskbar. Decorate it with trees and "
     "snowmen. Clicks always go straight through the snow.",
     "Снег падает на рабочий стол и ложится на окна и панель задач. Украсьте его ёлками и снеговиками. "
     "Щелчки всегда проходят сквозь снег.",
     "Сніг падає на робочий стіл і лягає на вікна та панель завдань. Прикрасьте його ялинками й "
     "сніговиками. Клацання завжди проходять крізь сніг."},
    {"Reset to defaults", "Сбросить настройки", "Скинути налаштування"},
    {"All settings return to their default values", "Все настройки вернутся к значениям по умолчанию",
     "Усі налаштування повернуться до типових значень"},
    {"Reset", "Сбросить", "Скинути"},
};

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(UiStr::Count),
              "string table out of sync with enum UiStr");
}  // namespace

const char* UiTr(UiStr id, Lang lang) {
  int i = static_cast<int>(id);
  if (i < 0 || i >= static_cast<int>(UiStr::Count)) return "";
  const Entry& e = kTable[i];
  switch (lang) {
    case Lang::Ru: return e.ru;
    case Lang::Uk: return e.uk;
    default: return e.en;
  }
}

std::string UiFormat(const std::string& fmt, std::initializer_list<std::string> args) {
  std::vector<std::string> a(args);
  std::string out;
  out.reserve(fmt.size() + 16);
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] == '{' && i + 2 < fmt.size() && fmt[i + 2] == '}' && fmt[i + 1] >= '0' && fmt[i + 1] <= '9') {
      size_t n = static_cast<size_t>(fmt[i + 1] - '0');
      if (n < a.size()) out += a[n];
      i += 2;
      continue;
    }
    out += fmt[i];
  }
  return out;
}

}  // namespace snow

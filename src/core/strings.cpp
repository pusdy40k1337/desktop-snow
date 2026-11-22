#include "strings.h"

namespace snow {

namespace {
struct Entry {
  const char* en;
  const char* ru;
  const char* uk;
};

// Order must match enum class Str.
const Entry kTable[] = {
    {"Desktop Snow", "Снег на рабочем столе", "Сніг на робочому столі"},
    {"New Year is already here.", "Новый год уже здесь.", "Новий рік уже тут."},
    {"Snow on", "Снег включён", "Сніг увімкнено"},
    {"Intensity", "Интенсивность", "Інтенсивність"},
    {"Light", "Лёгкий", "Легкий"},
    {"Normal", "Обычный", "Звичайний"},
    {"Heavy", "Сильный", "Сильний"},
    {"Blizzard", "Метель", "Хуртовина"},
    {"Effect", "Эффект", "Ефект"},
    {"Snow", "Снег", "Сніг"},
    {"Confetti", "Конфетти", "Конфеті"},
    {"Nothing falling", "Без осадков", "Без опадів"},
    {"Theme", "Тема", "Тема"},
    {"Classic", "Классическая", "Класична"},
    {"Frost", "Иней", "Іній"},
    {"Candy", "Леденцовая", "Льодяникова"},
    {"Golden night", "Золотая ночь", "Золота ніч"},
    {"Quick settings", "Быстрые настройки", "Швидкі налаштування"},
    {"Snow piles on windows", "Сугробы на окнах", "Кучугури на вікнах"},
    {"Snow on the taskbar", "Снег на панели задач", "Сніг на панелі завдань"},
    {"Wind gusts", "Порывы ветра", "Пориви вітру"},
    {"Cursor sweeps snow", "Курсор сметает снег", "Курсор змітає сніг"},
    {"Sparkling snow", "Искрящийся снег", "Іскристий сніг"},
    {"Slow melting", "Медленное таяние", "Повільне танення"},
    {"Day/night tint", "Оттенок дня и ночи", "Відтінок дня і ночі"},
    {"Reduced motion", "Меньше движения", "Менше руху"},
    {"Save power on battery", "Экономия на батарее", "Економія на батареї"},
    {"Visitors (Santa, train, yeti)", "Гости (Дед Мороз, поезд, йети)", "Гості (Святий Миколай, потяг, єті)"},
    {"Decorate…", "Украсить…", "Прикрасити…"},
    {"Surprise me", "Сюрприз", "Сюрприз"},
    {"Santa peeks in", "Заглянет Дед Мороз", "Загляне Святий Миколай"},
    {"Gift train", "Поезд с подарками", "Потяг із подарунками"},
    {"Yeti hunt", "Охота йети", "Полювання єті"},
    {"Clear snow", "Убрать снег", "Прибрати сніг"},
    {"Remove all decorations", "Убрать все украшения", "Прибрати всі прикраси"},
    {"Debug overlay", "Отладочный слой", "Налагоджувальний шар"},
    {"Language", "Язык", "Мова"},
    {"System", "Как в системе", "Як у системі"},
    {"Launch at startup", "Запускать при входе", "Запускати під час входу"},
    {"Open settings folder", "Открыть папку настроек", "Відкрити теку налаштувань"},
    {"Quit", "Выход", "Вийти"},
    {"Snow toggled", "Снег переключён", "Сніг перемкнено"},
    {"The hotkey is used by another app; use the tray menu instead.",
     "Горячая клавиша занята другим приложением — используйте меню в трее.",
     "Гаряча клавіша зайнята іншою програмою — скористайтеся меню в треї."},
    {"Pick a tool, click anywhere to place it, drag to move, scroll to resize. Hat or scarf on a snowman: click to pick a colour. Esc or right-click to finish.",
     "Выберите инструмент и щёлкните в любом месте — предмет появится там; перетащите, чтобы сдвинуть, колесо мыши меняет размер. Наведите шапку или шарф на снеговика, щёлкните — палитра цветов. Esc или правый щелчок — готово.",
     "Виберіть інструмент і клацніть будь-де — предмет з'явиться там; перетягніть, щоб зсунути, колесо миші змінює розмір. Наведіть шапку чи шарф на сніговика, клацніть — палітра кольорів. Esc або правий клік — готово."},
    {"Settings file was damaged; restored the last good copy.",
     "Файл настроек был повреждён; восстановлена последняя исправная копия.",
     "Файл налаштувань був пошкоджений; відновлено останню справну копію."},
    {"You can grow up to 3 trees.", "Можно вырастить не больше 3 ёлок.", "Можна виростити не більше 3 ялинок."},
    {"Tree", "Ёлка", "Ялинка"},
    {"Bauble", "Шар", "Кулька"},
    {"Lights", "Гирлянда", "Гірлянда"},
    {"Candy cane", "Леденец", "Льодяник"},
    {"Star", "Звезда", "Зірка"},
    {"Snowman", "Снеговик", "Сніговик"},
    {"Hat", "Шапка", "Шапка"},
    {"Scarf", "Шарф", "Шарф"},
    {"Remove", "Убрать", "Прибрати"},
    {"Done", "Готово", "Готово"},
    {"Paused", "На паузе", "На паузі"},
    {"Pause during fullscreen", "Пауза в полноэкранном режиме", "Пауза в повноекранному режимі"},
};

static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Str::Count),
              "string table out of sync with enum Str");
}  // namespace

const char* Tr(Str id, Lang lang) {
  int i = static_cast<int>(id);
  if (i < 0 || i >= static_cast<int>(Str::Count)) return "";
  const Entry& e = kTable[i];
  switch (lang) {
    case Lang::Ru: return e.ru;
    case Lang::Uk: return e.uk;
    default: return e.en;
  }
}

Lang ResolveLanguage(const std::string& setting, uint16_t systemPrimaryLangId) {
  if (setting == "en") return Lang::En;
  if (setting == "ru") return Lang::Ru;
  if (setting == "uk") return Lang::Uk;
  // LANG_RUSSIAN = 0x19, LANG_UKRAINIAN = 0x22
  if (systemPrimaryLangId == 0x19) return Lang::Ru;
  if (systemPrimaryLangId == 0x22) return Lang::Uk;
  return Lang::En;
}

const char* LangCode(Lang l) {
  switch (l) {
    case Lang::Ru: return "ru";
    case Lang::Uk: return "uk";
    default: return "en";
  }
}

}  // namespace snow

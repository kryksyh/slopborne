// bbport: Russian text of the in-game menu, keyed by its English text (bbport_overlay.cpp T()).
// Generated from the original Russian menu; keep in sync when menu text changes.
{"Bloodborne settings  (Insert / L3+R3)",
 "Bloodborne — настройки  (Insert / L3+R3)"},
{"Bloodborne settings  (Cmd+, / L3+R3)",
 "Bloodborne — настройки  (Cmd+, / L3+R3)"},
{"Temporal upscaler",
 "Временной апскейлер"},
{"Off",
 "Выкл"},
{"TAA (native anti-aliasing)",
 "TAA (нативное сглаживание)"},
{"Bilinear stretch (no temporal, softer)",
 "Билинейное растяжение (без временного, мягче)"},
{"Upscaler",
 "Апскейлер"},
{"— not supported by this GPU",
 "— не поддерживается видеокартой"},
{"— in progress",
 "— в работе"},
{"FSR 4 unavailable: %s",
 "FSR 4 недоступен: %s"},
{"The mode selected above is active. FSR 4 can be selected again.",
 "Активен режим, выбранный выше. FSR 4 можно выбрать снова."},
{"FSR 4.1.1 in INT8 mode: the AMD 4.1.1 DLL model reproduced in Vulkan (output matches the "
 "DLL). One model for Native..Performance and a separate one for Ultra Performance. Assets: "
 "tools/fsr4cap/build_assets.sh (needs the DLL and Proton).",
 "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат "
 "совпадает с DLL). Одна модель для Native..Performance и отдельная для Ultra Performance. "
 "Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и Proton)."},
{"FSR 4 in INT8 mode (model v07 from the AMD FidelityFX SDK sources). Better quality than FSR "
 "3.1, but a heavier pass. Changing the preset rebuilds the model (short pause). Assets: "
 "tools/fetch_fsr4_assets.sh.",
 "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, чем у FSR "
 "3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая пауза). Ассеты: "
 "tools/fetch_fsr4_assets.sh."},
{"FSR 4: auto exposure",
 "FSR 4: авто-экспозиция"},
{"FSR 4: inverted jitter sign",
 "FSR 4: обратный знак jitter"},
{"For ghosting checks: the FSR 4 network normalizes color by exposure and uses it to decide "
 "when to discard past frames. Applied immediately, no restart.",
 "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, когда "
 "отбросить прошлые кадры. Меняются сразу, без перезапуска."},
{"Preset",
 "Пресет"},
{"%s (x%.1f, render %dx%d)",
 "%s (x%.1f, рендер %dx%d)"},
{"TAA anti-aliases the scene at the output resolution, without the FSR model or upscaling. "
 "The saved FSR preset comes back when FSR is selected.",
 "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. Сохранённый пресет "
 "FSR восстановится при выборе FSR."},
{"Scene render size: %d x %d",
 "Активный рендер сцены: %d x %d"},
{"Preset at startup: %s",
 "Пресет при запуске: %s"},
{"With output other than 1080p the whole game renders at the preset resolution (patched at "
 "startup): fastest on Steam Deck and weaker GPUs. Preset and output resolution changes apply "
 "after a restart. \"Live resolution change\" in Advanced allows changes without a restart "
 "(post-processing then stays at 1080p, slower).",
 "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): это быстрее "
 "всего на Steam Deck и слабых GPU. Смена пресета или разрешения вывода — после перезапуска. "
 "Пункт «Смена разрешения на лету» в разделе «Дополнительно» включает смену без перезапуска (постобработка тогда "
 "остаётся в 1080p, медленнее)."},
{"BB_RENDER_RES fixes the scene size at startup. Remove this variable to change resolution "
 "and presets without restarting the game.",
 "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную для смены "
 "разрешения и пресетов без перезапуска игры."},
{"Native AA: FSR works as anti-aliasing. The other presets lower the scene render resolution "
 "relative to the output. The interface is drawn at the output resolution. The preset applies "
 "from the next frame, no restart.",
 "Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение отрисовки "
 "сцены относительно вывода. Интерфейс рисуется в разрешении вывода. Пресет применяется со "
 "следующего кадра без перезапуска игры."},
{"Sharpening (RCAS)",
 "Резкость (RCAS)"},
{"Sharpness",
 "Сила резкости"},
{"Up to 1: the upscaler's own sharpening (RCAS). Above 1 an extra RCAS pass is added. "
 "Ctrl+click the slider to type an exact value.",
 "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
 "Ctrl+клик по ползунку — ввести точное значение."},
{"Subpixel jitter",
 "Субпиксельный сдвиг (jitter)"},
{"Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more detail "
 "from several frames. Without it you only get history anti-aliasing.",
 "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких кадров "
 "больше деталей. Без него получается только сглаживание по истории."},
{"Reactive mask",
 "Маска реактивности"},
{"Enable mask",
 "Включить маску"},
{"Marks transparent effects (particles, haze) so the upscaler relies less on past frames. "
 "Fewer trails behind effects, but shimmering returns under them.",
 "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше опирался на прошлые "
 "кадры. Меньше шлейфов за эффектами, но под ними возвращается дрожание."},
{"Scale",
 "Масштаб"},
{"Threshold",
 "Порог"},
{"Maximum",
 "Максимум"},
{"Show mask (debug)",
 "Показать маску (отладка)"},
{"Character motion vectors",
 "Векторы движения персонажей"},
{"Exact vectors for animated objects: clothing and weapons break up less in motion. A static "
 "scene gets no extra pass. Applies after a game restart.",
 "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются при движении. "
 "Статичная сцена не получает дополнительный проход. Изменение применяется после перезапуска "
 "игры."},
{"Show motion vectors (debug)",
 "Показать векторы движения (отладка)"},
{"Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the pixel got an "
 "exact object vector, not just camera motion. A moving object with no blue and no red/green "
 "is treated as static by the upscaler, hence the trail.",
 "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). Синий: "
 "пиксель получил точный вектор объекта, а не только движение камеры. Движущийся предмет без "
 "синего и без красного/зелёного апскейлер считает неподвижным, отсюда шлейф."},
{"Output resolution",
 "Разрешение вывода"},
{"Size of the final frame and interface. The preset sets the scene size relative to the "
 "output: 4K Performance = 1920x1080. Applies after a game restart.",
 "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно вывода: 4K "
 "Performance = 1920x1080. Применяется после перезапуска игры."},
{"Size of the final frame and interface, changed at the next frame. The preset sets the scene "
 "size relative to the output: 4K Performance = 1920x1080. A size change resets the FSR "
 "history and may cause a short pause.",
 "Размер готового кадра и интерфейса меняется на границе следующего кадра. Пресет задаёт "
 "размер сцены относительно вывода: 4K Performance = 1920x1080. Смена размера сбрасывает "
 "историю FSR и может вызвать короткую паузу."},
{"1920 x 1080 (native)",
 "1920 x 1080 (родное)"},
{"Render resolution",
 "Разрешение рендера"},
{"Scene size at 1080p output, patched at game start: the smaller sizes are much faster and the "
 "upscaler (MetalFX on Apple GPUs) fills 1080p. Applies after a game restart.",
 "Размер сцены при выводе 1080p, задаётся патчем при запуске игры: меньшие размеры намного "
 "быстрее, а апскейлер (MetalFX на видеокартах Apple) дорисовывает до 1080p. Применяется после "
 "перезапуска игры."},
{"Auto (by GPU)",
 "Авто (по видеокарте)"},
{"Off (faster)",
 "Выключена (быстрее)"},
{"On",
 "Включена"},
{"Live resolution change",
 "Смена разрешения на лету"},
{"On: output resolution and preset change without a restart, but the game's post-processing "
 "stays at 1080p, noticeably slower on Steam Deck and older GPUs. Off: everything renders at "
 "the preset resolution, changes need a restart. Auto turns it on for powerful discrete GPUs. "
 "Applies after a game restart.",
 "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
 "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. Выключена: всё "
 "рисуется в разрешении пресета, смена — через перезапуск. Авто включает её на мощных "
 "дискретных видеокартах. Применяется после перезапуска игры."},
{"Game effects (after restart)",
 "Эффекты игры (после перезапуска)"},
{"Highest (-2)",
 "Максимальная (-2)"},
{"As in the game",
 "Как в игре"},
{"Lower (1)",
 "Ниже (1)"},
{"Lowest (2)",
 "Минимальная (2)"},
{"Model detail",
 "Детализация моделей"},
{"Effects are switched by game patches at startup (patches/Bloodborne.xml). Motion blur and "
 "dynamic light shadows are noticeably heavy on the GPU.",
 "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
 "Размытие в движении и тени от динамических источников заметно нагружают GPU."},
{"Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left touchpad / "
 "Tab. Needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from Nexus mod #253. Right "
 "touchpad: Backspace.",
 "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). Debug menu: "
 "левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font из мода "
 "Nexus #253. Правый touchpad: Backspace."},
{"Changes apply after a game restart",
 "Изменения применятся после перезапуска игры"},
{"Apply and restart the game",
 "Применить и перезапустить игру"},
{"Other",
 "Прочее"},
{"Experimental async shader compilation",
 "Экспериментальная фоновая компиляция шейдеров"},
{"Removes the stutter when something is seen for the first time: a new area, enemy "
 "or attack. The first time only, an enemy or character can show for a moment as a "
 "plain dark figure or not at all, an effect can be missing for a moment, and "
 "loading screens can take a little longer. Applies after a game restart.",
 "Убирает подтормаживания, когда что-то появляется впервые: новая локация, враг или "
 "атака. Только в первый раз враг или персонаж может на мгновение выглядеть как "
 "тёмная фигура или пропасть, эффект может ненадолго не отобразиться, а загрузки "
 "могут идти немного дольше. Применяется после перезапуска игры."},
{"FPS counter in the corner",
 "Счётчик FPS в углу"},
{"Close",
 "Закрыть"},
{"Settings are saved in bbport.ini",
 "Настройки сохраняются в bbport.ini"},
{"Chromatic aberration",
 "Хроматическая аберрация"},
{"Depth of field (DoF)",
 "Глубина резкости (DoF)"},
{"Motion blur",
 "Размытие в движении"},
{"Ambient occlusion (SSAO)",
 "Затенение SSAO"},
{"Game's own anti-aliasing",
 "Собственное сглаживание игры"},
{"Dynamic light shadows",
 "Тени от динамических источников"},
{"SSR reflections (not in the original game)",
 "Отражения SSR (не было в игре)"},
{"Skip intro videos",
 "Пропуск заставок при запуске"},
{"Free camera (Cross + L3)",
 "Свободная камера (Cross + L3)"},
{"Debug menu (needs font files)",
 "Debug menu (нужны файлы шрифтов)"},
{"Cheat: never die (HP stays at 1)",
 "Чит: бессмертие (не ниже 1 HP)"},
{"Cheat: enemies don't see you",
 "Чит: враги не замечают"},
{"Cheat: enemies don't hear you",
 "Чит: враги не слышат"},
{"Cheat: Rally never fades",
 "Чит: Rally не угасает"},
{"Cheat: control an enemy (R3 / L3)",
 "Чит: управление врагом (R3 / L3)"},
{"No Rally (HP regain)",
 "Без Rally (возврата HP)"},
{"Camera farther away",
 "Камера дальше"},
{"No camera auto-rotation",
 "Без автоповорота камеры"},
{"Run with less stick tilt",
 "Бег с меньшим наклоном стика"},
{"Dark Souls ragdoll physics",
 "Физика тел как в Dark Souls"},
{"No hitch every ~50 s (faster save compression)",
 "Без рывка каждые ~50 с (быстрое сжатие сохранений)"},
{"Quick loading after death and lamp travel",
 "Быстрая загрузка после смерти и у фонарей"},
{"Controller buttons", "Кнопки контроллера"},
{"No controller connected", "Контроллер не подключён"},
{"Button labels", "Обозначения кнопок"},
{"Automatic", "Автоматически"},
{"Positions", "По расположению"},
{"Bottom", "Нижняя"},
{"Right", "Правая"},
{"Left", "Левая"},
{"Top", "Верхняя"},
{"Cross", "Крест"},
{"Circle", "Круг"},
{"Square", "Квадрат"},
{"Triangle", "Треугольник"},
{"Steam or controller firmware can hide the printed layout. Choose Nintendo for Nintendo-style 8BitDo buttons.", "Steam или прошивка могут скрывать раскладку. Для 8BitDo с кнопками Nintendo выберите Nintendo."},
{"Game is still running while settings are open.", "Игра продолжает идти, пока открыты настройки."},
{"Step %d of 4: press and release the physical %s face button.", "Шаг %d из 4: нажмите и отпустите кнопку в позиции «%s»."},
{"Release the button to continue.", "Отпустите кнопку для продолжения."},
{"That input is not a new face button. Try again; calibration supports four distinct face-button inputs.", "Этот сигнал не соответствует новой лицевой кнопке. Нужны четыре разных сигнала лицевых кнопок."},
{"Cancel calibration", "Отменить калибровку"},
{"Calibrate remapped face buttons", "Откалибровать переназначенные кнопки"},
{"Reset calibration", "Сбросить калибровку"},
{"Calibration saved for this controller type.", "Калибровка сохранена для этого типа контроллера."},
{"Recalibrate after changing Steam or firmware mappings.", "После изменения назначений в Steam или прошивке повторите калибровку."},
{"Controller: [%s] = OK, [%s] = cancel", "Контроллер: [%s] = OK, [%s] = отмена"},
{"Options/Menu or Esc cancels calibration.", "Options/Menu или Esc отменяет калибровку."},
{"Button labels change the game glyphs after restart. Controls stay the same.", "Обозначения кнопок меняют значки в игре после перезапуска. Управление остаётся прежним."},

{"Display", "Изображение"},
{"Controls", "Управление"},
{"Gameplay", "Игра"},
{"Advanced", "Дополнительно"},
{"Shoulder buttons or Ctrl+Tab: switch page", "Бамперы или Ctrl+Tab: сменить раздел"},
{"Visual effects apply after restart.", "Визуальные эффекты применяются после перезапуска."},
{"Rendering experiments and diagnostic views. Leave these unchanged unless testing a specific issue.",
 "Экспериментальные настройки и диагностика. Меняйте их только для проверки конкретной проблемы."},
{"Resolution changes", "Изменение разрешения"},
{"Shader compilation", "Компиляция шейдеров"},
{"Temporal reconstruction", "Временная реконструкция"},
{"Developer tools (after restart)", "Инструменты разработчика (после перезапуска)"},
{"Gameplay preferences (after restart)", "Настройки игры (после перезапуска)"},
{"Cheats (after restart)", "Читы (после перезапуска)"},
{"Restart now? Progress since the last game save may be lost.",
 "Перезапустить сейчас? Прогресс после последнего сохранения игры может быть потерян."},
{"Restart now", "Перезапустить"},
{"Keep playing", "Продолжить игру"},
{"Quit game", "Выйти из игры"},
{"Press again to quit", "Нажмите ещё раз для выхода"},
{"No restart needed", "Перезапуск не требуется"},
{"Settings save automatically when this menu closes.", "Настройки сохраняются при закрытии этого меню."},
{"Frame rate (after restart)", "Частота кадров (после перезапуска)"},
{"30 FPS (as on PS4)", "30 FPS (как на PS4)"},
{"40 FPS", "40 FPS"},
{"60 FPS", "60 FPS"},
{"Uncapped (up to 120 FPS)", "Без ограничения (до 120 FPS)"},
{"Frame rate limit", "Ограничение частоты кадров"},
{"A rate the display's refresh divides evenly looks smoothest. Below 30 FPS the game runs in slow motion; above 120 its movement timing breaks.",
 "Плавнее всего частота, на которую ровно делится частота монитора. Ниже 30 FPS игра идёт в замедлении, выше 120 ломается тайминг движения."},

<h1>Kufar Telegram Notifier</h1>
<b>Kufar Telegram Notifier</b> - бот, который раз в заданное время ведет опрос официального Kufar API для поиска новых объявлений по заданным ключевым словам для последующей отправки результатов в Telegram чат.<br>
Использование данного бота предусматривается только для ознакомительных целей.<br><br>
<pre>Если Вы являетесь администрацией Kufar и у Вас имеются претензии к данному проекту - обращайтесь в раздел «<i>Issues</i>» для обратной связи.</pre>
<p align="center">
   <img src="https://user-images.githubusercontent.com/83237609/180989226-ec24b7d5-63ea-4ed5-9830-dd40d27ee30d.png" width="600"/>
</p>
<h2>Инструкция по сборке</h2>
<details>
   <summary>
      <h3>
         Linux
      </h3>
   </summary>
   <ol>
      <li>Установите g++: <code>sudo apt-get install g++</code></li>
      <li>
         Установите curl: <code>sudo apt-get install curl libcurl4-gnutls-dev</code>
      </li>
      <li>
         Установите make и cmake: <code>sudo apt-get install make cmake</code>
      </li>
      <li>
         <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/releases">Загрузите</a> исходный код последней версии программы:<br>
      </li>
      <li>
         Распакуйте архив с исходным кодом программы
      </li>
      <li>С помощью <code>cd</code> перейдите в директорию c проектом</li>
      <li>Соберите исполняемый файл с помощью:<br>
            <code>cmake .</code><br>
            <code>make</code>
      </li>
   </ol>
</details>
<details>
   <summary>
      <h3>macOS</h3>
   </summary>
   <details>
      <summary>Через Xcode (через графический интерфейс)</summary>
      <ol>
         <li>
            <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/releases">Загрузите</a> исходный код последней версии программы:<br>
         </li>
         <li>
            Распакуйте архив с исходным кодом программы
         </li>
         <li>
            Откройте <code>Kufar Telegram Notifier.xcodeproj</code> в Xcode
         </li>
         <li>
            В верхнем меню-баре выберите "Product" -> "Archive"
         </li>
         <li>
            В новом открывшемся окне нажмите "Distribute Content" -> "Built Products" -> "Next" -> "Export"
         </li>
      </ol>
   </details>
   <details>
      <summary>Через Xcode Command Line Tools (через консольный интерфейс)</summary>
      <ol>
         <li>
            Установите Xcode Command Line Tools: <code>xcode-select —install</code>
         </li>
         <li>
            <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/releases">Загрузите</a> исходный код последней версии программы<br>
         </li>
         <li>
            Распакуйте архив с исходным кодом программы
         </li>
         <li>С помощью <code>cd</code> перейдите в директорию c проектом</li>
        <li>Соберите исполняемый файл с помощью:<br>
            <code>cmake .</code><br>
            <code>make</code>
        </li>
      </ol>
   </details>
</details>
<h2>Конфигурация программы</h2>
<details>
   <summary>
      Настройка kufar-configuration.json
   </summary>
   <details>
      <summary>
         Telegram
      </summary>
      <b>bot-token</b> - токен вашего бота, который будет отправлять сообщения.<br>
      <b>chat-id</b> - идентификатор чата, в который будут отправляться сообщения.
   </details>
   <details>
      <summary>
         Queries
      </summary>
      <b>tag</b> - поисковой запрос. <sup>(Опционально)</sup><br>
      <b>only-title-search</b> - осуществление поиска только в заголовках. <sup>(Опционально)</sup>
      <details>
         <summary>
            Price <sup>(Опционально)</sup>
         </summary>
         <b>min</b> - минимальная цена (целочисленное значение в BYN). <sup>(Опционально)</sup><br>
         <b>max</b> - максимальная цена (целочисленное значение в BYN). <sup>(Опционально)</sup>
      </details>
      <b>language</b> - язык. <sup>(Опционально)</sup><br>
      <b>limit</b> - ограничение на количество получаемых объявлений за один запрос. <sup>(Опционально)</sup><br>
      <b>currency</b> - валюта <sup>(Опционально)</sup><br>
      <b>condition</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L515">состояние</a> (новое / б/y). <sup>(Опционально)</sup><br>
      <b>seller-type</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L520">тип продавца</a> (частное лицо / компания). <sup>(Опционально)</sup><br>
      <b>kufar-delivery-required</b> - только с Kufar Доставкой. <sup>(Опционально)</sup><br>
      <b>kufar-payment-required</b> - только с Kufar Оплатой. <sup>(Опционально)</sup><br>
      <b>kufar-halva-required</b> - только с Kufar Рассрочкой (Халва). <sup>(Опционально)</sup><br>
      <b>only-with-photos</b> - только с фото. <sup>(Опционально)</sup><br>
      <b>only-with-videos</b> - только с видео. <sup>(Опционально)</sup><br>
      <b>only-with-exchange-available</b> - только с возможностью обмена. <sup>(Опционально)</sup><br>
      <b>sort-type</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L525">тип сортировки</a>. <sup>(Опционально)</sup><br>
      <b>category</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L193">категория</a>. <sup>(Опционально)</sup><br>
      <b>sub-category</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L217">подкатегория</a>. <sup>(Опционально)</sup><br>
      <b>region</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L17">номер региона</a> для поиска объявлений. <sup>(Опционально)</sup><br>
      <b>areas</b> - <a href="https://github.com/TechUnRestricted/Kufar-Telegram-Notifier/blob/4e5eb51e3664c5e4e96812a5e146e41087787484/include/kufar.hpp#L27">номера областей</a> для поиска объявлений. <sup>(Опционально)</sup>
   </details>
   <details>
      <summary>
         Delays
      </summary>
      <b>query</b> - задержка (в секундах) перед переходом к следующему поисковому запросу.<br>
      <b>loop</b> - задержка (в секундах) перед повторением поиска по очереди с начала.<br>
   </details>
</details>
<details>
     <summary>
         Настройка cached-data.json
     </summary>
Настраивать данный файл не нужно.<br>
Достаточно убедиться в том, что он представляет из себя валидный JSON файл со структурой <code>[]</code> (массив).<br>
Предназначение: хранит в себе идентификаторы отправленных объявлений для предотвращения повторной отправки при перезапуске программы.
</details>
<h2>Запуск:</h2>
   <p>
      По умолчанию <code>kufar-configuration.json</code>, <code>cached-data.json</code> и <code>kufar-notifier.log</code>
      ищутся (а при необходимости создаются) в текущей рабочей папке. Пути можно переопределить аргументами командной строки.
   </p>
   <details>
      <summary>
         Аргументы командной строки
      </summary>
      <code>bin/Kufar-Telegram-Notifier [параметры]</code><br><br>
      <b>-c, --config &lt;файл&gt;</b> - путь к файлу конфигурации.<br>
      <b>--cache &lt;файл&gt;</b> - путь к файлу кеша (создаётся автоматически, если отсутствует).<br>
      <b>-l, --log &lt;файл&gt;</b> - путь к файлу лога.<br>
      <b>--no-log</b> - не записывать лог в файл (только консоль).<br>
      <b>--log-level &lt;уровень&gt;</b> - уровень логирования: <code>trace</code>, <code>debug</code>, <code>info</code>, <code>warn</code>, <code>error</code>, <code>critical</code>.<br>
      <b>-V, --version</b> - показать версию программы.<br>
      <b>-h, --help</b> - показать справку.
   </details>
   <details>
      <summary>
         Примеры запуска
      </summary>
      <code>bin/Kufar-Telegram-Notifier</code> - файлы по умолчанию в текущей папке.<br>
      <code>bin/Kufar-Telegram-Notifier --config="/путь/к/kufar-configuration.json" --cache="/путь/к/cached-data.json"</code><br>
      <code>bin/Kufar-Telegram-Notifier --log=/путь/к/kufar.log --log-level=debug</code>
   </details>
<p align="center">
   <img src="https://user-images.githubusercontent.com/83237609/181288185-7f9c23b0-32bf-4a1a-a3fd-168ed38255e1.png"/>
</p>

// Example bilingual blog entry: the origin post, written when the project was
// still a three-epoch MVP backed by a Google Sheets spreadsheet.
// Usage: mongosh boat_rudder scripts/seed/blog-origin-post.js
//
// Idempotent: re-running replaces the document with link "estoy-construyendo-un-cms-retrocompatible".
//
// Dated 2023 on purpose: it is a period document, and it says THREE epochs
// because that is what the design had at the time. The five-epoch scheme and
// the MongoDB backend came later - see /blog/boat-rudder-launch.

const LINK   = 'estoy-construyendo-un-cms-retrocompatible';
const IMAGE  = '/content/posts/theretrocenter.com/Docs/cms-retrocompatible.jpg';
const AUTHOR = db.users.findOne({ email: 'theretrocenter.com@gmail.com' });
const NEWS   = db.entry_categories.findOne({ 'name.en': 'News' });

if (!AUTHOR) throw new Error('seed user not found');
if (!NEWS)   throw new Error('"News" category not found');

// Paragraph text is deliberately plain, with no inline HTML: the same string is
// poured into a WML card, a bare <p> and a styled epoch-3 <p>, and raw markup
// would be invalid in the first of those.
const blocks = [
  { type: 'title', extra_data: '1', text: {
    en: 'I am building a retro-compatible CMS',
    es: 'Estoy construyendo un CMS retrocompatible' } },

  { type: 'paragraph', extra_data: 'lead', text: {
    en: 'My name is Jonathan Pablo Toledo M. I am a Computer Science engineer with technical training in electrical work, and I keep a collection of old computers - machines from the early 80s that still switch on.',
    es: 'Me llamo Jonathan Pablo Toledo M. Soy Ingeniero en Ciencias de la Computación, con estudios técnicos en electricidad, y tengo una colección de computadoras antiguas: máquinas de principios de los 80 que todavía encienden.' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'My work has mostly been the web. The first professional site I built was in 2004, though I had been programming for a while before that - around 1999 I was writing software in a BASIC interpreter. But my biggest contributions have always been on the web side. I started when browsers barely managed text and a couple of images.',
    es: 'Mi trabajo ha sido, sobre todo, la web. El primer sitio profesional que hice fue en 2004, aunque ya venía programando desde antes: por ahí por 1999 escribía software en un intérprete de BASIC. Pero mis mayores contribuciones han estado siempre del lado de la web. Partí cuando los navegadores apenas mostraban texto y un par de imágenes.' } },

  { type: 'title', extra_data: '2', text: {
    en: 'Capable machines, cut off from everything',
    es: 'Máquinas capaces, pero incomunicadas' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'That is why it saddens me that machines with 16-bit processors, machines that once connected to networks, are today completely cut off. It is not that they lack capability: they have more than enough power to receive information and put it on screen. What they lack is a site on the other end willing to speak a language they understand. The web stopped writing to them, not the other way around.',
    es: 'Por eso me da pena que máquinas con procesadores de 16 bits, que alguna vez pudieron conectarse a redes, hoy estén completamente incomunicadas. No es que les falte capacidad: tienen potencia de sobra para recibir información e imprimirla en pantalla. Lo que les falta es un sitio del otro lado dispuesto a hablarles en un idioma que entiendan. La web dejó de escribirles a ellas, no al revés.' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'So I am taking my 25-plus years of experience and building a system to bring these devices out of isolation. Not emulation, not a museum replica: the same website, served in a shape a machine from 1985 can actually read.',
    es: 'Así que, tomando mi experiencia de más de 25 años, decidí construir un sistema que permita revivir y sacar del aislamiento a estos interesantes dispositivos. No emulación, ni una copia de museo: la misma web, servida de una forma que un equipo de 1985 pueda leer de verdad.' } },

  { type: 'title', extra_data: '2', text: {
    en: 'First stage: a three-epoch MVP',
    es: 'La primera etapa: un MVP de tres épocas' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'The starting plan is deliberately small: an MVP with templates split into three epochs, and a spreadsheet-style database to publish content from.',
    es: 'El plan para empezar es acotado a propósito: un MVP con plantillas divididas en tres épocas y una base de datos tipo planilla desde la cual publicar contenidos.' } },

  // A language-neutral caption: extra_data is untranslated, and epochs -1/0
  // render the caption *instead of* the image.
  { type: 'image', extra_data: 'EPOCH 1, EPOCH 2, EPOCH 3 + SPREADSHEET|100|center', text: {
    en: IMAGE, es: IMAGE } },

  { type: 'list', extra_data: '', text: {
    en: 'Epoch 1, early - browsers from the first years, which get the basic HTML template\nEpoch 2, middle - browsers that understand CSS1 and CSS2, which get the styled template\nEpoch 3, modern - browsers with CSS3 and HTML5, which get the current template',
    es: 'Época 1, temprana: navegadores de los primeros años, que reciben la plantilla de HTML básico\nÉpoca 2, media: navegadores compatibles con CSS1 y CSS2, que reciben la plantilla con estilos\nÉpoca 3, moderna: navegadores con CSS3 y HTML5, que reciben la plantilla actual' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'The server reads the User Agent that arrives, decides which epoch it belongs to, and assembles the page out of that epoch\'s pieces. Every component - the container, the menu, the slider, the blog - exists three times, once per epoch, and an orchestrator combines them according to what the visitor can understand. Theme and language travel down the same path, so a single engine can serve a dark theme and a light one.',
    es: 'El servidor lee el User Agent que llega, decide a qué época pertenece y arma la página con las piezas de esa época. Cada componente (el contenedor, el menú, el slider, el blog) existe tres veces, una por época, y un orquestador los combina según lo que el visitante pueda entender. El tema y el idioma viajan por el mismo camino, así que un solo motor puede servir un tema oscuro y uno claro.' } },

  { type: 'separator', extra_data: '', text: { en: '', es: '' } },

  { type: 'title', extra_data: '2', text: {
    en: 'Why a spreadsheet?',
    es: '¿Por qué una planilla de cálculo?' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'Because for an MVP I do not need a real database, I need to be able to write. Routes, front-page content, blog summaries and full entries all live for now in a Google Sheets spreadsheet, and the server reads it through its API. It is a temporary decision and I know it, but it lets me concentrate on what actually matters right now: getting the page assembled correctly for each epoch. The database can always be swapped out later. The template engine is the hard part.',
    es: 'Porque para un MVP no necesito una base de datos de verdad, necesito poder escribir. Las rutas, el contenido de la portada, los resúmenes del blog y las entradas completas viven por ahora en una planilla de Google Sheets, y el servidor la consulta a través de su API. Es una decisión temporal y lo sé, pero me deja concentrarme en lo que de verdad importa ahora: que la página se arme bien para cada época. La base de datos siempre se puede cambiar después. El motor de plantillas es lo difícil.' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'It is written in C, with no heavy dependencies, so it can run just about anywhere. If this goes the way I hope, at some point I will be able to open my own site from one of the machines on the shelf and read it end to end. That is the whole goal.',
    es: 'Está escrito en C, sin dependencias pesadas, para que pueda correr prácticamente en cualquier parte. Si esto sale como espero, en algún momento voy a poder abrir mi propio sitio desde una de las máquinas del estante y leerlo completo. Ese es todo el objetivo.' } },

  { type: 'link', extra_data: '/page/about', text: {
    en: 'Read more about the project',
    es: 'Conoce más sobre el proyecto' } }
];

const doc = {
  link: LINK,
  type: 'blog',
  enabled: true,
  categories: [NEWS._id],
  header: {
    image_url: IMAGE,
    title: {
      en: 'I am building a retro-compatible CMS',
      es: 'Estoy construyendo un CMS retrocompatible'
    },
    summary: {
      en: 'I keep a collection of computers from the 1980s that can no longer reach the web. After 25 years building websites, I decided to build the system that brings them back into the conversation - starting with three epochs and a spreadsheet.',
      es: 'Tengo una colección de computadoras de los años 80 que ya no pueden salir a la web. Después de 25 años haciendo sitios, decidí construir el sistema que las devuelva a la conversación, partiendo por tres épocas y una planilla de cálculo.'
    },
    date: ISODate('2023-08-23T00:00:00.000Z'),
    author_id: AUTHOR._id,
    hide_author: false
  },
  content: blocks.map(function (b, i) {
    return { _id: new ObjectId(), type: b.type, order: i, text: b.text, extra_data: b.extra_data };
  })
};

db.entries.deleteOne({ link: LINK });
const res = db.entries.insertOne(doc);
print('inserted ' + res.insertedId + '  ->  /blog/' + LINK + '  (' + doc.content.length + ' blocks)');

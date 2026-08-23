// Example bilingual blog entry: the Boat Rudder launch announcement.
// Usage: mongosh boat_rudder scripts/seed/blog-launch-post.js
//
// Idempotent: re-running replaces the document with link "boat-rudder-launch".
//
// NOTE: the Spanish text is written with proper accents, which are UTF-8. Epochs
// -1/0/1 currently serve "text/html" with no charset and their layouts declare
// none, so those bytes reach a 90s browser as Latin-1. See the charset note in
// the review that shipped with this file.

const LINK   = 'boat-rudder-launch';
const IMAGE  = '/content/posts/theretrocenter.com/Docs/boat-rudder-launch.jpg';
const AUTHOR = db.users.findOne({ email: 'theretrocenter.com@gmail.com' });
const NEWS   = db.entry_categories.findOne({ 'name.en': 'News' });

if (!AUTHOR) throw new Error('seed user not found');
if (!NEWS)   throw new Error('"News" category not found');

// Paragraph text is deliberately plain, with no inline HTML: the same string is
// poured into a WML card, a bare <p> and a styled epoch-3 <p>, and raw markup
// would be invalid in the first of those.
const blocks = [
  { type: 'tittle', extra_data: '1', text: {
    en: 'Boat Rudder is here: one website, every browser',
    es: 'Llega Boat Rudder: un solo sitio, todos los navegadores' } },

  { type: 'paragraph', extra_data: 'lead', text: {
    en: 'Boat Rudder, a content manager built for the whole history of the web, is officially out. The idea behind it is simple: the same site should open on a phone from 1999, on a text-only browser, and on whatever you are reading this with right now. Not a stripped-down copy parked at a separate address, but the same articles, the same navigation, and the same pictures wherever pictures can be shown.',
    es: 'Boat Rudder, un gestor de contenidos pensado para toda la historia de la web, ya está disponible. La idea de fondo es simple: el mismo sitio debería abrirse en un teléfono de 1999, en un navegador de solo texto y en aquello con lo que estás leyendo esto ahora mismo. No una copia recortada estacionada en otra dirección, sino los mismos artículos, la misma navegación y las mismas fotos allí donde se puedan mostrar.' } },

  // The caption is deliberately language-neutral: extra_data is untranslated,
  // and epochs -1/0 render the caption *instead of* the image, so an empty one
  // would leave a bare "[Image: ]" on text-only clients.
  { type: 'image', extra_data: 'Boat Rudder: WML, TEXT, HTML 3.2, HTML4+CSS, HTML5+CSS3|100|center', text: {
    en: IMAGE, es: IMAGE } },

  { type: 'tittle', extra_data: '2', text: {
    en: 'Five versions of every page',
    es: 'Cinco versiones de cada página' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'When someone arrives, Boat Rudder reads what their browser says about itself and picks one of five ways to build the page. A WAP phone from the early 2000s gets a simple card it can actually display. A text browser gets clean headings and links with no decoration. A mid-90s browser gets a table-based layout. A browser from the 2000s gets basic styling. Everyone else gets the modern, responsive version.',
    es: 'Cuando alguien llega, Boat Rudder lee lo que su navegador dice de sí mismo y elige una de cinco maneras de construir la página. Un teléfono WAP de comienzos de los 2000 recibe una ficha simple que sí puede mostrar. Un navegador de texto recibe títulos y enlaces limpios, sin decoración. Un navegador de mediados de los 90 recibe una maquetación con tablas. Uno de los 2000 recibe estilos básicos. El resto recibe la versión moderna y adaptable.' } },

  { type: 'list', extra_data: '', text: {
    en: 'WAP phones - a simple card, text and links only\nText browsers such as Lynx - headings and links, nothing else\nMid-90s browsers - table layouts, no stylesheets\nEarly 2000s browsers - simple styling\nBrowsers of today - the full modern layout',
    es: 'Teléfonos WAP - una ficha simple, solo texto y enlaces\nNavegadores de texto como Lynx - títulos y enlaces, nada más\nNavegadores de mediados de los 90 - maquetación con tablas, sin hojas de estilo\nNavegadores de comienzos de los 2000 - estilos básicos\nNavegadores de hoy - la maquetación moderna completa' } },

  { type: 'separator', extra_data: '', text: { en: '', es: '' } },

  { type: 'tittle', extra_data: '2', text: {
    en: 'Nothing simply disappears',
    es: 'Nada desaparece sin más' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'The interesting part is what happens to the things an old browser cannot handle. A video does not vanish: it becomes a printed QR code that a phone can scan. A photo gallery becomes a list of links. A large picture becomes a lighter one that a 1994 machine can actually load, and following it opens the full-size file. The article keeps its shape everywhere; only the presentation changes.',
    es: 'Lo interesante es qué pasa con aquello que un navegador antiguo no puede manejar. Un video no se esfuma: se convierte en un código QR impreso que un teléfono puede escanear. Una galería de fotos se vuelve una lista de enlaces. Una imagen grande se convierte en una más liviana que una máquina de 1994 sí puede cargar, y al seguirla se abre el archivo completo. El artículo mantiene su forma en todas partes; solo cambia la presentación.' } },

  { type: 'tittle', extra_data: '2', text: {
    en: 'Written once, read in two languages',
    es: 'Escrito una vez, leído en dos idiomas' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'Every article is stored with its translations side by side, so offering a second language does not mean maintaining a second website. This very post is stored as a single document and reads natively in both English and Spanish. Adding a third language is a matter of filling in the blanks, not of copying anything.',
    es: 'Cada artículo se guarda con sus traducciones juntas, así que ofrecer un segundo idioma no significa mantener un segundo sitio. Esta misma nota está guardada como un solo documento y se lee de forma nativa en inglés y en español. Agregar un tercer idioma es cuestión de completar los espacios en blanco, no de copiar nada.' } },

  { type: 'tittle', extra_data: '2', text: {
    en: 'Who is it for?',
    es: '¿Para quién es?' } },

  { type: 'paragraph', extra_data: '', text: {
    en: 'Boat Rudder was built for retro computing enthusiasts who want their own site reachable from the machines they collect, and for anyone who likes the idea of a web page that does not stop working just because it got old. It runs as a single small program, so it is comfortable on modest hardware.',
    es: 'Boat Rudder fue creado para entusiastas de la computación retro que quieren que su propio sitio sea alcanzable desde las máquinas que coleccionan, y para cualquiera a quien le guste la idea de una página web que no deja de funcionar solo por haberse vuelto vieja. Corre como un único programa pequeño, así que se siente cómodo en hardware modesto.' } },

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
      en: 'Boat Rudder is here: one website, every browser',
      es: 'Llega Boat Rudder: un solo sitio, todos los navegadores'
    },
    summary: {
      en: 'A new content manager serves the same site to a text browser from 1994 and to the browser you are using today - no plugins, no cut-down mobile copy, and no "please upgrade your browser".',
      es: 'Un nuevo gestor de contenidos entrega el mismo sitio a un navegador de texto de 1994 y al navegador que usas hoy: sin plugins, sin versión móvil recortada y sin el clásico "actualiza tu navegador".'
    },
    date: ISODate('2026-08-23T00:00:00.000Z'),
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

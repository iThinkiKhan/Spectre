// Registers the stub/extension resolve hook, then runs the .ts test cases
// against the real companion-app source. Usage: node verify-enrich/run.mjs
import {register} from 'node:module';
register('./loader.mjs', import.meta.url);
await import('./cases.ts');

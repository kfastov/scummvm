// Кладёт стартовый конфиг ScummVM в Documents при первом запуске.
//
// Зачем: список игр ScummVM читает из Documents/Preferences, а IPA физически
// не может создать там файлы — контейнер приложения iOS создаёт пустым.
// Конструктор dylib отрабатывает в dyld до main(), то есть раньше, чем
// ScummVM вызовет createConfigReadStream().
//
// Всё поведение — best-effort: при любой неожиданности просто ничего не делаем
// и даём приложению стартовать как обычно. Уронить запуск мы права не имеем.

#import <Foundation/Foundation.h>

__attribute__((constructor))
static void scummvm_seed_preferences(void) {
	@autoreleasepool {
		@try {
			NSFileManager *fm = [NSFileManager defaultManager];

			NSURL *docs = [[fm URLsForDirectory:NSDocumentDirectory
			                          inDomains:NSUserDomainMask] firstObject];
			if (docs == nil) {
				return;
			}
			NSString *dst = [[docs path] stringByAppendingPathComponent:@"Preferences"];

			// Пользователь уже что-то настроил — не перетираем.
			if ([fm fileExistsAtPath:dst]) {
				return;
			}

			NSString *res = [[NSBundle mainBundle] resourcePath];
			if (res == nil) {
				return;
			}
			NSString *src = [res stringByAppendingPathComponent:@"Preferences.default"];
			if (![fm fileExistsAtPath:src]) {
				return;
			}

			NSError *err = nil;
			if (![fm copyItemAtPath:src toPath:dst error:&err]) {
				NSLog(@"[seed] не удалось скопировать конфиг: %@", err);
			}
		} @catch (__unused NSException *e) {
			// Намеренно проглатываем: неудачный сид лучше, чем несработавший запуск.
		}
	}
}

// Prueba básica: sin conexión la app abre vacía, sin datos inventados, y muestra las 3 pestañas.
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:banco/main.dart';

void main() {
  testWidgets('La app abre y muestra Monitoreo, Historial y Ajustes', (tester) async {
    SharedPreferences.setMockInitialValues({}); // sin IP guardada
    await tester.pumpWidget(const BmsApp());
    await tester.pump();
    expect(find.text('Banco de Baterías'), findsWidgets);
    expect(find.text('Monitoreo'), findsOneWidget);
    expect(find.text('Historial'), findsWidgets);
    expect(find.text('Ajustes'), findsOneWidget);
    expect(find.text('Relevadores'), findsOneWidget);
    expect(find.text('Desconectado'), findsWidgets);
    expect(find.text('— %'), findsWidgets);
    expect(find.text('DEMO'), findsNothing);
  });
}

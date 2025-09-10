/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import java.lang.annotation.Retention;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;

import static java.lang.annotation.RetentionPolicy.RUNTIME;

@Retention(RUNTIME)
@interface InnerAnno {
    int q() default 0;
    int r() default 0;
    String s();
}

@Retention(RUNTIME)
@interface FooAnno {
    int x() default 0;
    String y() default "y";
    InnerAnno inner();
}

@Retention(RUNTIME)
@interface BarAnno {
    String a() default "a";
    String[] z();
}

@FooAnno(x = 123, y = "this is a class", inner = @InnerAnno(q = 99, s = "this is unused"))
class FooClass {
    @BarAnno(a = "this is a field", z = {"C", "E", "G"})
    public short s;

    @BarAnno(a = "this is a method", z = {"F", "A", "C", "E"})
    public char m() {
        return 'm';
    }
}

public class RemoveAnnotationElementsTest {

    private static void dump(Object o) {
        android.util.Log.w("RemoveAnnotationElementsTest", o == null ? "NULL" : o.toString());
    }

    public static void dumpStuffToLogcat() throws NoSuchFieldException, NoSuchMethodException {
        // Make some but not all members used
        dump("x:");
        dump(FooClass.class.getAnnotation(FooAnno.class).x());
        dump("q:");
        dump(FooClass.class.getAnnotation(FooAnno.class).inner().q());
        FooClass f = new FooClass();
        dump(f.s);
        dump(f.m());

        Field s = FooClass.class.getDeclaredField("s");
        dump("a:");
        dump(s.getAnnotation(BarAnno.class).a());

        Method m = FooClass.class.getDeclaredMethod("m");
        dump("a:");
        dump(m.getAnnotation(BarAnno.class).a());
    }

    @Test
    public void testReadAnnotationValues() throws NoSuchFieldException, NoSuchMethodException {
        dumpStuffToLogcat();
        assertThat(FooClass.class.getAnnotation(FooAnno.class).x()).isEqualTo(123);
        assertThat(FooClass.class.getAnnotation(FooAnno.class).inner().q()).isEqualTo(99);

        Field s = FooClass.class.getDeclaredField("s");
        assertThat(s.getAnnotation(BarAnno.class).a()).isEqualTo("this is a field");

        Method m = FooClass.class.getDeclaredMethod("m");
        assertThat(m.getAnnotation(BarAnno.class).a()).isEqualTo("this is a method");
    }
}

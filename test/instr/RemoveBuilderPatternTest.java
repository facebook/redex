// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

package com.facebook.redex.test.instr;

import org.junit.Test;
import java.util.BitSet;
import android.support.annotation.Nullable;
import static org.fest.assertions.api.Assertions.assertThat;

import com.facebook.litho.Column;
import com.facebook.litho.Component;
import com.facebook.litho.ComponentContext;
import com.facebook.litho.SpecGeneratedComponent;
import com.facebook.litho.annotations.Prop;
import com.facebook.litho.annotations.ResType;
import com.facebook.litho.sections.SectionContext;

class LithoComponent extends SpecGeneratedComponent implements Cloneable {

  @Prop(
    resType = ResType.NONE,
    optional = false
  )
  int prop1;

  private LithoComponent(ComponentContext context) {
    super("LithoComponent");
  }

  public static Builder create(ComponentContext context) {
    return create(context, 0, 0);
  }

  public static Builder create(ComponentContext context, int defStyleAttr, int defStyleRes) {
    final Builder builder = new Builder();
    LithoComponent instance = new LithoComponent(context);
    builder.init(context, defStyleAttr, defStyleRes, instance);
    return builder;
  }

  public static class Builder extends Component.Builder<Builder> {
    @Nullable LithoComponent mLithoComponent;
    ComponentContext mContext;

    private final String[] REQUIRED_PROPS_NAMES = new String[] {"prop1"};
    private final int REQUIRED_PROPS_COUNT = 1;
    private final BitSet mRequired = new BitSet(REQUIRED_PROPS_COUNT);

    private void init(
        ComponentContext context,
        int defStyleAttr,
        int defStyleRes,
        LithoComponent lithoComponentRef) {
      super.init(context, defStyleAttr, defStyleRes, lithoComponentRef);
      mLithoComponent = lithoComponentRef;
      mContext = context;
      mRequired.clear();
    }

    @Override
    protected void setComponent(Component component) {
      mLithoComponent = (LithoComponent) component;
    }

    public Builder prop1(int prop1) {
      this.mLithoComponent.prop1 = prop1;
      mRequired.set(0);
      return this;
    }

    @Override
    public LithoComponent build() {
      checkArgs(REQUIRED_PROPS_COUNT, mRequired, REQUIRED_PROPS_NAMES);
      return mLithoComponent;
    }

    @Override
    public Builder getThis() {
      return this;
    }
  }
}

class LithoComponentWithStaticFields extends SpecGeneratedComponent implements Cloneable {

  @Prop(
    resType = ResType.NONE,
    optional = false
  )
  int prop1;

  private LithoComponentWithStaticFields(ComponentContext context) {
    super("LithoComponentWithStaticFields");
  }

  public static Builder create(ComponentContext context) {
    return create(context, 0, 0);
  }

  public static Builder create(ComponentContext context, int defStyleAttr, int defStyleRes) {
    final Builder builder = new Builder();
    LithoComponentWithStaticFields instance = new LithoComponentWithStaticFields(context);
    builder.init(context, defStyleAttr, defStyleRes, instance);
    return builder;
  }

  public static class Builder extends Component.Builder<Builder> {
    @Nullable LithoComponentWithStaticFields mLithoComponent;
    ComponentContext mContext;

    private final String[] REQUIRED_PROPS_NAMES = new String[] {"prop1"};
    private final int REQUIRED_PROPS_COUNT = 1;
    private final BitSet mRequired = new BitSet(REQUIRED_PROPS_COUNT);
    public static boolean RANDOM_INPUT;

    private void init(
        ComponentContext context,
        int defStyleAttr,
        int defStyleRes,
        LithoComponentWithStaticFields lithoComponentRef) {
      super.init(context, defStyleAttr, defStyleRes, lithoComponentRef);
      mLithoComponent = lithoComponentRef;
      mContext = context;
      mRequired.clear();
    }

    @Override
    protected void setComponent(Component component) {
      mLithoComponent = (LithoComponentWithStaticFields) component;
    }

    public Builder prop1(int prop1) {
      this.mLithoComponent.prop1 = prop1;
      mRequired.set(0);
      return this;
    }

    @Override
    public LithoComponentWithStaticFields build() {
      checkArgs(REQUIRED_PROPS_COUNT, mRequired, REQUIRED_PROPS_NAMES);
      return mLithoComponent;
    }

    @Override
    public Builder getThis() {
      return this;
    }
  }
}

class TestingComponentA extends SpecGeneratedComponent implements Cloneable {

  @Prop(
    resType = ResType.NONE,
    optional = false
  )
  int prop1;

  private TestingComponentA(ComponentContext context) {
    super("TestingComponentA");
  }

  public static Builder create(ComponentContext context) {
    return create(context, 0, 0);
  }

  public static Builder create(ComponentContext context, int defStyleAttr, int defStyleRes) {
    final Builder builder = new Builder();
    TestingComponentA instance = new TestingComponentA(context);
    builder.init(context, defStyleAttr, defStyleRes, instance);
    return builder;
  }

  public static class Builder extends Component.Builder<Builder> {
    @Nullable TestingComponentA mLithoComponent;
    ComponentContext mContext;

    private final String[] REQUIRED_PROPS_NAMES = new String[] {"prop1"};
    private final int REQUIRED_PROPS_COUNT = 1;
    private final BitSet mRequired = new BitSet(REQUIRED_PROPS_COUNT);

    private void init(
        ComponentContext context,
        int defStyleAttr,
        int defStyleRes,
        TestingComponentA lithoComponentRef) {
      super.init(context, defStyleAttr, defStyleRes, lithoComponentRef);
      mLithoComponent = lithoComponentRef;
      mContext = context;
      mRequired.clear();
    }

    @Override
    protected void setComponent(Component component) {
      mLithoComponent = (TestingComponentA) component;
    }

    public Builder prop1(int prop1) {
      this.mLithoComponent.prop1 = prop1;
      mRequired.set(0);
      return this;
    }

    @Override
    public TestingComponentA build() {
      checkArgs(REQUIRED_PROPS_COUNT, mRequired, REQUIRED_PROPS_NAMES);
      return mLithoComponent;
    }

    @Override
    public Builder getThis() {
      return this;
    }
  }
}

class TestingComponentB extends SpecGeneratedComponent implements Cloneable {

  @Prop(
    resType = ResType.NONE,
    optional = false
  )
  int prop1;

  private TestingComponentB(ComponentContext context) {
    super("TestingComponentB");
  }

  public static Builder create(ComponentContext context) {
    return create(context, 0, 0);
  }

  public static Builder create(ComponentContext context, int defStyleAttr, int defStyleRes) {
    final Builder builder = new Builder();
    TestingComponentB instance = new TestingComponentB(context);
    builder.init(context, defStyleAttr, defStyleRes, instance);
    return builder;
  }

  public static class Builder extends Component.Builder<Builder> {
    @Nullable TestingComponentB mLithoComponent;
    ComponentContext mContext;

    private final String[] REQUIRED_PROPS_NAMES = new String[] {"prop1"};
    private final int REQUIRED_PROPS_COUNT = 1;
    private final BitSet mRequired = new BitSet(REQUIRED_PROPS_COUNT);

    private void init(
        ComponentContext context,
        int defStyleAttr,
        int defStyleRes,
        TestingComponentB lithoComponentRef) {
      super.init(context, defStyleAttr, defStyleRes, lithoComponentRef);
      mLithoComponent = lithoComponentRef;
      mContext = context;
      mRequired.clear();
    }

    @Override
    protected void setComponent(Component component) {
      mLithoComponent = (TestingComponentB) component;
    }

    public Builder prop1(int prop1) {
      this.mLithoComponent.prop1 = prop1;
      mRequired.set(0);
      return this;
    }

    @Override
    public TestingComponentB build() {
      checkArgs(REQUIRED_PROPS_COUNT, mRequired, REQUIRED_PROPS_NAMES);
      return mLithoComponent;
    }

    @Override
    public Builder getThis() {
      return this;
    }
  }
}

class TestingComponentC extends SpecGeneratedComponent implements Cloneable {

  @Prop(
    resType = ResType.NONE,
    optional = false
  )
  int prop1;

  private TestingComponentC(ComponentContext context) {
    super("TestingComponentC");
  }

  public static Builder create(ComponentContext context) {
    return create(context, 0, 0);
  }

  public static Builder create(ComponentContext context, int defStyleAttr, int defStyleRes) {
    final Builder builder = new Builder();
    TestingComponentC instance = new TestingComponentC(context);
    builder.init(context, defStyleAttr, defStyleRes, instance);
    return builder;
  }

  public static class Builder extends Component.Builder<Builder> {
    @Nullable TestingComponentC mLithoComponent;
    ComponentContext mContext;

    private final String[] REQUIRED_PROPS_NAMES = new String[] {"prop1"};
    private final int REQUIRED_PROPS_COUNT = 1;
    private final BitSet mRequired = new BitSet(REQUIRED_PROPS_COUNT);

    private void init(
        ComponentContext context,
        int defStyleAttr,
        int defStyleRes,
        TestingComponentC lithoComponentRef) {
      super.init(context, defStyleAttr, defStyleRes, lithoComponentRef);
      mLithoComponent = lithoComponentRef;
      mContext = context;
      mRequired.clear();
    }

    @Override
    protected void setComponent(Component component) {
      mLithoComponent = (TestingComponentC) component;
    }

    public Builder prop1(int prop1) {
      this.mLithoComponent.prop1 = prop1;
      mRequired.set(0);
      return this;
    }

    @Override
    public TestingComponentC build() {
      checkArgs(REQUIRED_PROPS_COUNT, mRequired, REQUIRED_PROPS_NAMES);
      return mLithoComponent;
    }

    @Override
    public Builder getThis() {
      return this;
    }
  }
}

class TestingComponentD extends SpecGeneratedComponent implements Cloneable {

  @Prop(
    resType = ResType.NONE,
    optional = false
  )
  int prop1;

  private TestingComponentD(ComponentContext context) {
    super("TestingComponentD");
  }

  public static Builder create(ComponentContext context) {
    return create(context, 0, 0);
  }

  public static Builder create(ComponentContext context, int defStyleAttr, int defStyleRes) {
    final Builder builder = new Builder();
    TestingComponentD instance = new TestingComponentD(context);
    builder.init(context, defStyleAttr, defStyleRes, instance);
    return builder;
  }

  public static class Builder extends Component.Builder<Builder> {
    @Nullable TestingComponentD mLithoComponent;
    ComponentContext mContext;

    private final String[] REQUIRED_PROPS_NAMES = new String[] {"prop1"};
    private final int REQUIRED_PROPS_COUNT = 1;
    private final BitSet mRequired = new BitSet(REQUIRED_PROPS_COUNT);

    private void init(
        ComponentContext context,
        int defStyleAttr,
        int defStyleRes,
        TestingComponentD lithoComponentRef) {
      super.init(context, defStyleAttr, defStyleRes, lithoComponentRef);
      mLithoComponent = lithoComponentRef;
      mContext = context;
      mRequired.clear();
    }

    @Override
    protected void setComponent(Component component) {
      mLithoComponent = (TestingComponentD) component;
    }

    public Builder prop1(int prop1) {
      this.mLithoComponent.prop1 = prop1;
      mRequired.set(0);
      return this;
    }

    @Override
    public @Nullable TestingComponentD build() {
      checkArgs(REQUIRED_PROPS_COUNT, mRequired, REQUIRED_PROPS_NAMES);
      return mLithoComponent;
    }

    @Override
    public Builder getThis() {
      return this;
    }
  }
}

class Model {
  public int field;

  public static Builder newBuilder() {
    return new Builder();
  }

  public static class Builder {
    int field = 0;

    public void set_field(int f) {
      field = f;
    }

    public Model build() {
      Model obj = new Model();
      obj.field = field;
      return obj;
    }
  }
}

class TestBuilder {

  Component.Builder mBuilder;

  public void testRemoveBuilder(SectionContext context, boolean random) {
    LithoComponent.Builder builder = LithoComponent.create(context);
    if (random) {
      builder.prop1(7);
    } else {
      builder.prop1(8);
    }
    builder.build();
  }

  public void testRemoveBuilderAllocationWithStaticFields(SectionContext context, boolean random) {
    LithoComponentWithStaticFields.Builder builder = LithoComponentWithStaticFields.create(context);
    LithoComponentWithStaticFields.Builder.RANDOM_INPUT = random;
    if (LithoComponentWithStaticFields.Builder.RANDOM_INPUT) {
      builder.prop1(7);
    } else {
      builder.prop1(8);
    }
    builder.build();
  }

  public void testWhenCheckIfNull(ComponentContext c_context, SectionContext context, boolean random) {
    final Column.Builder container_builder = Column.create(c_context);
    LithoComponent.Builder builder = LithoComponent.create(context);
    if (random) {
      builder.prop1(7);
    } else {
      builder.prop1(8);
    }
    container_builder.child(builder);
  }

  public void testRemoveIfConditionallyCreated(
      ComponentContext c_context, SectionContext context, boolean random) {
    final Column.Builder container_builder = Column.create(c_context);
    container_builder.child(
      random
        ? LithoComponent.create(context)
            .prop1(7)
        : null);
  }

  public void testRemoveIfCopy(
      ComponentContext c_context, SectionContext context, int random) {
    final Column.Builder container_builder = Column.create(c_context);
    LithoComponent.Builder builder1 = null;
    if (random == 1) {
      LithoComponent.Builder builder2 = builder1;
      if (random == 2) {
        builder1 = LithoComponent.create(context);
        builder2 = LithoComponent.create(context);
      }
      container_builder.child(builder2);
    }
    container_builder.child(builder1);
  }

  public void nonRemovedIfDifferentInstancesCreated(
      ComponentContext c_context, SectionContext context, boolean random) {
    Component.Builder builder;
    if (random) {
      builder = TestingComponentA.create(context).prop1(7);
    } else {
      builder = TestingComponentB.create(context).prop1(8);
    }
    builder.build();
  }

  public void nonRemovedIfStored(
      ComponentContext c_context, SectionContext context, boolean random) {
    TestingComponentA.Builder builder = TestingComponentA.create(context);
    builder.prop1(7);

    // store created builder
    mBuilder = builder;
  }

  public void removeIfUsedInAConditionBranch(SectionContext context) {
    // Builder used in this test should not escape in other function!
    Component.Builder builder1 = LithoComponent.create(context);
    Component.Builder builder2 = LithoComponent.create(context);
    if (builder1 != builder2) {
      builder1.build();
      builder2.build();
    }
  }

  public void notRemovedIfUsedForSynchronization(
      ComponentContext c_context, SectionContext context, boolean random) {
    Component.Builder builder = TestingComponentC.create(context);
    synchronized(builder) {
      builder.build();
    }
  }

  public Component.Builder nonRemovedIfReturned(
      ComponentContext c_context, SectionContext context, boolean random) {
    TestingComponentB.Builder builder_B = TestingComponentB.create(context);
    builder_B.prop1(8);
    builder_B.build();

    return builder_B;
  }

  public Component removeBuilderForNotNullCheck(
      ComponentContext c_context, SectionContext context) {
    LithoComponent.Builder builder = LithoComponent.create(context);
    int val;

    // NOTE: This actually generates an IF_NEZ instruction ...
    if (builder == null) {
      val = 7;
    } else {
      val = 8;
    }
    builder.prop1(val);
    return builder.build();
  }

  public Component removeBuilderForNullCheck(
      ComponentContext c_context, SectionContext context) {
    LithoComponent.Builder builder = LithoComponent.create(context);
    int val = 8;

    // NOTE: This actually generates an IF_EQZ instruction ...
    if (builder != null) {
      val = 7;
    }
    builder.prop1(val);
    return builder.build();
  }

  public void nonRemovedIfInstanceOfUsed(SectionContext context) {
    // Builder used in this test should not escape in other function!
    TestingComponentD.Builder builder = TestingComponentD.create(context);
    if (builder instanceof TestingComponentD.Builder) {
      ((TestingComponentD.Builder) builder).prop1(7);
    }
    builder.build();
  }
}

public class RemoveBuilderPatternTest {
  @Test
  public void testRemoveSimpleBuilder() {
    Model.Builder builder = Model.newBuilder();
    int data = 1;
    builder.set_field(data);
    Model obj = builder.build();
    assertThat(obj.field).isEqualTo(1);
  }
}
